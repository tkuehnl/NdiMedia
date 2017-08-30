// Copyright 1998-2016 Epic Games, Inc. All Rights Reserved.

#include "NdiMediaPlayer.h"
#include "NdiMediaPrivate.h"

#include "HAL/PlatformProcess.h"
#include "IMediaAudioSink.h"
#include "IMediaBinarySink.h"
#include "IMediaOptions.h"
#include "IMediaTextureSink.h"
#include "Misc/ScopeLock.h"
#include "NdiMediaAudioSampler.h"
#include "NdiMediaSettings.h"
#include "NdiMediaSource.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/WeakObjectPtr.h"

#include "NdiMediaAllowPlatformTypes.h"


#define LOCTEXT_NAMESPACE "FNdiMediaPlayer"


/* FNdiVideoPlayer structors
 *****************************************************************************/

FNdiMediaPlayer::FNdiMediaPlayer()
	: AudioSink(nullptr)
	, MetadataSink(nullptr)
	, VideoSink(nullptr)
	, SelectedAudioTrack(INDEX_NONE)
	, SelectedMetadataTrack(INDEX_NONE)
	, SelectedVideoTrack(INDEX_NONE)
	, AudioSampler(new FNdiMediaAudioSampler)
	, CurrentState(EMediaState::Closed)
	, LastAudioChannels(0)
	, LastAudioSampleRate(0)
	, LastBufferDim(FIntPoint::ZeroValue)
	, LastVideoDim(FIntPoint::ZeroValue)
	, LastVideoFrameRate(0.0f)
	, Paused(false)
	, ReceiverInstance(nullptr)
	, VideoSinkFormat(EMediaTextureSinkFormat::CharUYVY)
{
	AudioSampler->OnSamples().BindRaw(this, &FNdiMediaPlayer::HandleAudioSamplerSample);
}


FNdiMediaPlayer::~FNdiMediaPlayer()
{
	Close();

	AudioSampler->OnSamples().Unbind();
	delete AudioSampler;
	AudioSampler = nullptr;
}


/* IMediaControls interface
 *****************************************************************************/

FTimespan FNdiMediaPlayer::GetDuration() const
{
	return FTimespan::Zero();
//	return (CurrentState == EMediaState::Playing) ? FTimespan::MaxValue() : FTimespan::Zero();
}


float FNdiMediaPlayer::GetRate() const
{
	return (CurrentState == EMediaState::Playing) ? 1.0f : 0.0f;
}


EMediaState FNdiMediaPlayer::GetState() const
{
	return CurrentState;
}


TRange<float> FNdiMediaPlayer::GetSupportedRates(EMediaPlaybackDirections Direction, bool Unthinned) const
{
	return TRange<float>(1.0f);
}


FTimespan FNdiMediaPlayer::GetTime() const
{
	return FTimespan::Zero();
//	return (CurrentState == EMediaState::Playing) ? FTimespan::MaxValue() : FTimespan::Zero();
}


bool FNdiMediaPlayer::IsLooping() const
{
	return false; // not supported
}


bool FNdiMediaPlayer::Seek(const FTimespan& Time)
{
	return false; // not supported
}


bool FNdiMediaPlayer::SetLooping(bool Looping)
{
	return false; // not supported
}


bool FNdiMediaPlayer::SetRate(float Rate)
{
	if (Rate == 0.0f)
	{
		Paused = true;
	}
	else if (Rate == 1.0f)
	{
		Paused = false;
	}
	else
	{
		return false;
	}

	return true;
}


bool FNdiMediaPlayer::SupportsRate(float Rate, bool Unthinned) const
{
	return (Rate == 1.0f);
}


bool FNdiMediaPlayer::SupportsScrubbing() const
{
	return false; // not supported
}


bool FNdiMediaPlayer::SupportsSeeking() const
{
	return false; // not supported
}


/* IMediaPlayer interface
 *****************************************************************************/

void FNdiMediaPlayer::Close()
{
	{
		FScopeLock Lock(&CriticalSection);

		if (ReceiverInstance != nullptr)
		{
			NDIlib_recv_destroy(ReceiverInstance);
			ReceiverInstance = nullptr;
		}

		CurrentState = EMediaState::Closed;
		CurrentUrl.Empty();

		LastAudioChannels = 0;
		LastAudioSampleRate = 0;
		LastBufferDim = FIntPoint::ZeroValue;
		LastVideoDim = FIntPoint::ZeroValue;
		LastVideoFrameRate = 0.0f;

		SelectedAudioTrack = INDEX_NONE;
		SelectedMetadataTrack = INDEX_NONE;
		SelectedVideoTrack = INDEX_NONE;
	}

	UpdateAudioSampler();

	MediaEvent.Broadcast(EMediaEvent::TracksChanged);
	MediaEvent.Broadcast(EMediaEvent::MediaClosed);
}


IMediaControls& FNdiMediaPlayer::GetControls()
{
	return *this;
}


FString FNdiMediaPlayer::GetInfo() const
{
	return FString(); // @todo gmp: implement NDI info
}


FName FNdiMediaPlayer::GetName() const
{
	static FName PlayerName(TEXT("NdiMedia"));
	return PlayerName;
}


IMediaOutput& FNdiMediaPlayer::GetOutput()
{
	return *this;
}


FString FNdiMediaPlayer::GetStats() const
{
	NDIlib_recv_performance_t PerfDropped, PerfTotal;
	NDIlib_recv_get_performance(ReceiverInstance, &PerfTotal, &PerfDropped);

	NDIlib_recv_queue_t Queue;
	NDIlib_recv_get_queue(ReceiverInstance, &Queue);

	FString StatsString;
	{
		StatsString += TEXT("Total Frames\n");
		StatsString += FString::Printf(TEXT("    Audio: %i\n"), PerfTotal.audio_frames);
		StatsString += FString::Printf(TEXT("    Video: %i\n"), PerfTotal.video_frames);
		StatsString += FString::Printf(TEXT("    Metadata: %i\n"), PerfTotal.metadata_frames);
		StatsString += TEXT("\n");

		StatsString += TEXT("Dropped Frames\n");
		StatsString += FString::Printf(TEXT("    Audio: %i\n"), PerfDropped.audio_frames);
		StatsString += FString::Printf(TEXT("    Video: %i\n"), PerfDropped.video_frames);
		StatsString += FString::Printf(TEXT("    Metadata: %i\n"), PerfDropped.metadata_frames);
		StatsString += TEXT("\n");

		StatsString += TEXT("Queue Depth\n");
		StatsString += FString::Printf(TEXT("    Audio: %i\n"), Queue.audio_frames);
		StatsString += FString::Printf(TEXT("    Video: %i\n"), Queue.video_frames);
		StatsString += FString::Printf(TEXT("    Metadata: %i\n"), Queue.metadata_frames);
		StatsString += TEXT("\n");
	}

	return StatsString;
}


IMediaTracks& FNdiMediaPlayer::GetTracks()
{
	return *this;
}


FString FNdiMediaPlayer::GetUrl() const
{
	return CurrentUrl;
}


bool FNdiMediaPlayer::Open(const FString& Url, const IMediaOptions& Options)
{
	Close();

	if (Url.IsEmpty() || !Url.StartsWith(TEXT("ndi://")))
	{
		return false;
	}

	FString SourceStr = Url.RightChop(6);

	// determine sink format
	auto ColorFormat = (NDIlib_recv_color_format_e)Options.GetMediaOption(NdiMedia::ColorFormatOption, 0LL);

	if (ColorFormat == NDIlib_recv_color_format_e_BGRX_BGRA)
	{
		VideoSinkFormat = EMediaTextureSinkFormat::CharBGRA;
	}
	else if (ColorFormat == NDIlib_recv_color_format_e_UYVY_BGRA)
	{
		VideoSinkFormat = EMediaTextureSinkFormat::CharUYVY;
	}
	else
	{
		UE_LOG(LogNdiMedia, Warning, TEXT("Unsupported ColorFormat option in media source %s. Falling back to UYVY."), *SourceStr);

		ColorFormat = NDIlib_recv_color_format_e_UYVY_BGRA;
		VideoSinkFormat = EMediaTextureSinkFormat::CharUYVY;
	}

	// create receiver
	int64 Bandwidth = Options.GetMediaOption(NdiMedia::BandwidthOption, (int64)NDIlib_recv_bandwidth_highest);

	NDIlib_source_t Source;
	{
		if (SourceStr.Find(TEXT(":")) != INDEX_NONE)
		{
			Source.p_ip_address = TCHAR_TO_ANSI(*SourceStr);
			Source.p_ndi_name = nullptr;
		}
		else
		{
			if (SourceStr.StartsWith(TEXT("localhost ")))
			{
				SourceStr.ReplaceInline(TEXT("localhost"), FPlatformProcess::ComputerName());
			}

			Source.p_ip_address = nullptr;
			Source.p_ndi_name = TCHAR_TO_ANSI(*SourceStr);
		}
	}

	NDIlib_recv_create_t RcvCreateDesc;
	{
		RcvCreateDesc.source_to_connect_to = Source;
		RcvCreateDesc.color_format = ColorFormat;
		RcvCreateDesc.bandwidth = (NDIlib_recv_bandwidth_e)Bandwidth;
		RcvCreateDesc.allow_video_fields = true;
	};

	FScopeLock Lock(&CriticalSection);

	ReceiverInstance = NDIlib_recv_create_v2(&RcvCreateDesc);

	if (ReceiverInstance == nullptr)
	{
		UE_LOG(LogNdiMedia, Error, TEXT("Failed to open NDI media source %s: couldn't create receiver"), *SourceStr);

		return false;
	}

	// send product metadata
	auto Settings = GetDefault<UNdiMediaSettings>();

	SendMetadata(
		FString::Printf(TEXT("<ndi_product short_name=\"%s\" long_name=\"%s\" manufacturer=\"%s\" version=\"%s\" serial_number=\"%s\" session_name=\"%s\" />"),
			*Settings->ProductName,
			*Settings->ProductDescription,
			*Settings->Manufacturer,
			*Settings->GetVersionName(),
			*Settings->SerialNumber,
			*Settings->SessionName
	));

	// send format metadata
	FString AudioFormatString;
	FString VideoFormatString;

	const int64 AudioChannels = Options.GetMediaOption(NdiMedia::AudioChannelsOption, (int64)0);
	
	if (AudioChannels > 0)
	{
		AudioFormatString += FString::Printf(TEXT(" no_channels=\"%i\""), AudioChannels);
	}

	const int64 AudioSampleRate = Options.GetMediaOption(NdiMedia::AudioSampleRateOption, (int64)0);

	if (AudioSampleRate > 0)
	{
		AudioFormatString += FString::Printf(TEXT(" sample_rate=\"%i\""), AudioSampleRate);
	}

	const int64 FrameRateD = Options.GetMediaOption(NdiMedia::FrameRateDOption, (int64)0);

	if (FrameRateD > 0)
	{
		VideoFormatString += FString::Printf(TEXT(" frame_rate_d=\"%i\""), FrameRateD);
	}

	const int64 FrameRateN = Options.GetMediaOption(NdiMedia::FrameRateDOption, (int64)0);

	if (FrameRateN > 0)
	{
		VideoFormatString += FString::Printf(TEXT(" frame_rate_n=\"%i\""), FrameRateN);
	}

	const FString Progressive = Options.GetMediaOption(NdiMedia::ProgressiveOption, FString());

	if (!Progressive.IsEmpty())
	{
		VideoFormatString += FString::Printf(TEXT(" progressive=\"%s\""), *Progressive);
	}

	const int64 VideoHeight = Options.GetMediaOption(NdiMedia::VideoHeightOption, (int64)0);

	if (VideoHeight > 0)
	{
		VideoFormatString += FString::Printf(TEXT(" yres=\"%i\""), VideoHeight);
	}

	const int64 VideoWidth = Options.GetMediaOption(NdiMedia::VideoWidthOption, (int64)0);

	if (VideoWidth > 0)
	{
		VideoFormatString += FString::Printf(TEXT(" xres=\"%i\""), VideoWidth);
	}

	if (!AudioFormatString.IsEmpty() || !VideoFormatString.IsEmpty())
	{
		SendMetadata(
			FString::Printf(TEXT("<ndi_format><audio_format %s /><video_format %s /></ndi_format>"),
				*AudioFormatString,
				*VideoFormatString
		));
	}

	// send custom metadata
	FString CustomMetadata = Settings->CustomMetaData;
	{
		CustomMetadata.Trim();
		CustomMetadata.TrimTrailing();
	}

	if (!CustomMetadata.IsEmpty())
	{
		SendMetadata(CustomMetadata);
	}

	// finalize
	CurrentUrl = Url;

	MediaEvent.Broadcast(EMediaEvent::TracksChanged);
	MediaEvent.Broadcast(EMediaEvent::MediaOpened);

	return true;
}


bool FNdiMediaPlayer::Open(const TSharedRef<FArchive, ESPMode::ThreadSafe>& Archive, const FString& OriginalUrl, const IMediaOptions& Options)
{
	return false; // not supported
}


void FNdiMediaPlayer::TickPlayer(float DeltaTime)
{
	if (ReceiverInstance == nullptr)
	{
		return;
	}

	// update player state
	const bool IsConnected = (NDIlib_recv_get_no_connections(ReceiverInstance) > 0);
	const EMediaState State = Paused ? EMediaState::Paused : (IsConnected ? EMediaState::Playing : EMediaState::Preparing);

	if ((State != CurrentState) && (AudioSink != nullptr))
	{
		CurrentState = State;
		UpdateAudioSampler();

		if (State == EMediaState::Playing)
		{
			MediaEvent.Broadcast(EMediaEvent::PlaybackResumed);

			if (AudioSink != nullptr)
			{
				AudioSink->ResumeAudioSink();
			}
		}
		else
		{
			MediaEvent.Broadcast(EMediaEvent::PlaybackSuspended);

			if (AudioSink != nullptr)
			{
				AudioSink->PauseAudioSink();
				AudioSink->FlushAudioSink();
			}
		}
	}

	if (MetadataSink != nullptr)
	{
		CaptureMetadataFrame();
	}
}


void FNdiMediaPlayer::TickVideo(float DeltaTime)
{
	if (!Paused)
	{
		CaptureVideoFrame();
	}
}


/* IMediaOutput interface
 *****************************************************************************/

void FNdiMediaPlayer::SetAudioSink(IMediaAudioSink* Sink)
{
	if (Sink == AudioSink)
	{
		return;
	}

	FScopeLock Lock(&CriticalSection);

	if (AudioSink != nullptr)
	{
		AudioSink->ShutdownAudioSink();
	}

	if (Sink != nullptr)
	{
		Sink->InitializeAudioSink(LastAudioChannels, LastAudioSampleRate);
	}

	AudioSink = Sink;

	UpdateAudioSampler();
}


void FNdiMediaPlayer::SetMetadataSink(IMediaBinarySink* Sink)
{
	if (Sink == MetadataSink)
	{
		return;
	}

	if (MetadataSink != nullptr)
	{
		MetadataSink->ShutdownBinarySink();
	}

	if (Sink != nullptr)
	{
		Sink->InitializeBinarySink();
	}

	MetadataSink = Sink;
}


void FNdiMediaPlayer::SetOverlaySink(IMediaOverlaySink* Sink)
{
	// not supported
}


void FNdiMediaPlayer::SetVideoSink(IMediaTextureSink* Sink)
{
	if (Sink == VideoSink)
	{
		return;
	}

	FScopeLock Lock(&CriticalSection);

	if (VideoSink != nullptr)
	{
		VideoSink->ShutdownTextureSink();
	}

	VideoSink = Sink;

	if (Sink != nullptr)
	{
		Sink->InitializeTextureSink(LastVideoDim, LastBufferDim, VideoSinkFormat, EMediaTextureSinkMode::Unbuffered);
	}
}


/* IMediaTracks interface
 *****************************************************************************/

uint32 FNdiMediaPlayer::GetAudioTrackChannels(int32 TrackIndex) const
{
	if ((ReceiverInstance == nullptr) || (TrackIndex != 0))
	{
		return 0;
	}

	return LastAudioChannels;
}


uint32 FNdiMediaPlayer::GetAudioTrackSampleRate(int32 TrackIndex) const
{
	if ((ReceiverInstance == nullptr) || (TrackIndex != 0))
	{
		return 0;
	}

	return LastAudioSampleRate;
}


int32 FNdiMediaPlayer::GetNumTracks(EMediaTrackType TrackType) const
{
	if (ReceiverInstance != nullptr)
	{
		if ((TrackType == EMediaTrackType::Audio) ||
			(TrackType == EMediaTrackType::Metadata) ||
			(TrackType == EMediaTrackType::Video))
		{
			return 1;
		}
	}

	return 0;
}


int32 FNdiMediaPlayer::GetSelectedTrack(EMediaTrackType TrackType) const
{
	if (ReceiverInstance == nullptr)
	{
		return INDEX_NONE;
	}

	switch (TrackType)
	{
	case EMediaTrackType::Audio:
	case EMediaTrackType::Metadata:
	case EMediaTrackType::Video:
		return 0;

	default:
		return INDEX_NONE;
	}
}


FText FNdiMediaPlayer::GetTrackDisplayName(EMediaTrackType TrackType, int32 TrackIndex) const
{
	if ((ReceiverInstance == nullptr) || (TrackIndex != 0))
	{
		return FText::GetEmpty();
	}

	switch (TrackType)
	{
	case EMediaTrackType::Audio:
		return LOCTEXT("DefaultAudioTrackName", "Audio Track");

	case EMediaTrackType::Metadata:
		return LOCTEXT("DefaultMetadataTrackName", "Metadata Track");

	case EMediaTrackType::Video:
		return LOCTEXT("DefaultVideoTrackName", "Video Track");

	default:
		return FText::GetEmpty();
	}
}


FString FNdiMediaPlayer::GetTrackLanguage(EMediaTrackType TrackType, int32 TrackIndex) const
{
	if ((ReceiverInstance == nullptr) || (TrackIndex != 0))
	{
		return FString();
	}

	return TEXT("und");
}


FString FNdiMediaPlayer::GetTrackName(EMediaTrackType TrackType, int32 TrackIndex) const
{
	return FString();
}


uint32 FNdiMediaPlayer::GetVideoTrackBitRate(int32 TrackIndex) const
{
	return 0;
}


FIntPoint FNdiMediaPlayer::GetVideoTrackDimensions(int32 TrackIndex) const
{
	if ((ReceiverInstance == nullptr) || (TrackIndex != 0))
	{
		return FIntPoint::ZeroValue;
	}

	return LastVideoDim;
}


float FNdiMediaPlayer::GetVideoTrackFrameRate(int32 TrackIndex) const
{
	if ((ReceiverInstance == nullptr) || (TrackIndex != 0))
	{
		return 0;
	}

	return LastVideoFrameRate;
}


bool FNdiMediaPlayer::SelectTrack(EMediaTrackType TrackType, int32 TrackIndex)
{
	if ((TrackIndex != INDEX_NONE) && (TrackIndex != 0))
	{
		return false;
	}

	if (TrackType == EMediaTrackType::Audio)
	{
		SelectedAudioTrack = TrackIndex;
		UpdateAudioSampler();
	}
	else if (TrackType == EMediaTrackType::Metadata)
	{
		SelectedMetadataTrack = TrackIndex;
	}
	else if (TrackType == EMediaTrackType::Video)
	{
		SelectedVideoTrack = TrackIndex;
	}
	else
	{
		return false;
	}

	return true;
}


/* FNdiMediaPlayer implementation
 *****************************************************************************/

void FNdiMediaPlayer::CaptureMetadataFrame()
{
	NDIlib_metadata_frame_t MetadataFrame;
	NDIlib_frame_type_e FrameType = NDIlib_recv_capture_v2(ReceiverInstance, nullptr, nullptr, &MetadataFrame, 0);

	if (FrameType == NDIlib_frame_type_error)
	{
		UE_LOG(LogNdiMedia, Verbose, TEXT("Failed to receive metadata frame"));
		return;
	}

	if (FrameType != NDIlib_frame_type_metadata)
	{
		return;
	}

	MetadataSink->ProcessBinarySinkData((const uint8*)MetadataFrame.p_data, MetadataFrame.length, FTimespan(MetadataFrame.timecode), FTimespan::Zero());

	NDIlib_recv_free_metadata(ReceiverInstance, &MetadataFrame);
}


void FNdiMediaPlayer::CaptureVideoFrame()
{
	NDIlib_video_frame_v2_t VideoFrame;
	NDIlib_frame_type_e FrameType = NDIlib_recv_capture_v2(ReceiverInstance, &VideoFrame, nullptr, nullptr, 0);

	if (FrameType == NDIlib_frame_type_error)
	{
		UE_LOG(LogNdiMedia, Verbose, TEXT("Failed to receive video frame"));
		return;
	}

	if (FrameType != NDIlib_frame_type_video)
	{
		return;
	}

	// re-initialize sink if format changed
	FScopeLock Lock(&CriticalSection);
	ProcessVideoFrame(VideoFrame);

	NDIlib_recv_free_video_v2(ReceiverInstance, &VideoFrame);
}


void FNdiMediaPlayer::ProcessAudioFrame(const NDIlib_audio_frame_v2_t& AudioFrame)
{
	LastAudioChannels = AudioFrame.no_channels;
	LastAudioSampleRate = AudioFrame.sample_rate;

	if (AudioSink == nullptr)
	{
		return;
	}

	// re-initialize sink if format changed
	if ((AudioSink->GetAudioSinkChannels() != AudioFrame.no_channels) ||
		(AudioSink->GetAudioSinkSampleRate() != AudioFrame.sample_rate))
	{
		if (!AudioSink->InitializeAudioSink(AudioFrame.no_channels, AudioFrame.sample_rate))
		{
			return;
		}
	}

	// convert float samples to interleaved 16-bit samples
	uint32 TotalSamples = AudioFrame.no_samples * AudioFrame.no_channels;

	NDIlib_audio_frame_interleaved_16s_t AudioFrameInterleaved = { 0 };
	{
		AudioFrameInterleaved.reference_level = 20;
		AudioFrameInterleaved.p_data = new short[TotalSamples];
	}

	NDIlib_util_audio_to_interleaved_16s_v2(&AudioFrame, &AudioFrameInterleaved);

	// forward to sink
	static int64 SamplesReceived = 0;
	SamplesReceived += TotalSamples;
	AudioSink->PlayAudioSink((const uint8*)AudioFrameInterleaved.p_data, TotalSamples * sizeof(int16), FTimespan(AudioFrame.timecode));

	delete[] AudioFrameInterleaved.p_data;
}


void FNdiMediaPlayer::ProcessVideoFrame(const NDIlib_video_frame_v2_t& VideoFrame)
{
	LastBufferDim = FIntPoint(VideoFrame.line_stride_in_bytes / 4, VideoFrame.yres);
	LastVideoDim = FIntPoint(VideoFrame.xres, VideoFrame.yres);

	if (VideoSink == nullptr)
	{
		return;
	}

	// re-initialize sink if format changed
	if ((VideoSink->GetTextureSinkFormat() != VideoSinkFormat) ||
		(VideoSink->GetTextureSinkDimensions() != LastVideoDim))
	{
		if (!VideoSink->InitializeTextureSink(LastVideoDim, LastBufferDim, VideoSinkFormat, EMediaTextureSinkMode::Unbuffered))
		{
			return;
		}
	}

	// forward to sink
	VideoSink->UpdateTextureSinkBuffer(VideoFrame.p_data, VideoFrame.line_stride_in_bytes);
	VideoSink->DisplayTextureSinkBuffer(FTimespan(VideoFrame.timecode));
}


void FNdiMediaPlayer::SendMetadata(const FString& Metadata, int64 Timecode)
{
	check(ReceiverInstance != nullptr);

	NDIlib_metadata_frame_t MetadataFrame;
	{
		MetadataFrame.length = Metadata.Len() + 1;
		MetadataFrame.timecode = Timecode;
		MetadataFrame.p_data = TCHAR_TO_ANSI(*Metadata);
	}

	NDIlib_recv_add_connection_metadata(ReceiverInstance, &MetadataFrame);
}


void FNdiMediaPlayer::UpdateAudioSampler()
{
	const bool SampleAudio = !Paused && (AudioSink != nullptr) && (SelectedAudioTrack == 0);
	AudioSampler->SetReceiverInstance(SampleAudio ? ReceiverInstance : nullptr);
}


/* FNdiMediaPlayer implementation
 *****************************************************************************/

void FNdiMediaPlayer::HandleAudioSamplerSample(const NDIlib_audio_frame_v2_t& AudioFrame)
{
	FScopeLock Lock(&CriticalSection);
	ProcessAudioFrame(AudioFrame);
}


#undef LOCTEXT_NAMESPACE


#include "NdiMediaHidePlatformTypes.h"
