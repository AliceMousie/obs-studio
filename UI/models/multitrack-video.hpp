/*
 * Copyright (c) 2024 Ruwen Hahn <haruwenz@twitch.tv>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#pragma once

#include <string>
#include <optional>

#include <obs.h>

#include <nlohmann/json.hpp>

/*
 * Support for (de-)serialising std::optional
 * From https://github.com/nlohmann/json/issues/1749#issuecomment-1731266676
 * whatsnew.hpp's version doesn't seem to work here
 */
template<typename T> struct nlohmann::adl_serializer<std::optional<T>> {
	static void from_json(const json &j, std::optional<T> &opt)
	{
		if (j.is_null()) {
			opt = std::nullopt;
		} else {
			opt = j.get<T>();
		}
	}
	static void to_json(json &json, std::optional<T> t)
	{
		if (t) {
			json = *t;
		} else {
			json = nullptr;
		}
	}
};

NLOHMANN_JSON_SERIALIZE_ENUM(obs_scale_type,
			     {
				     {OBS_SCALE_DISABLE, "OBS_SCALE_DISABLE"},
				     {OBS_SCALE_POINT, "OBS_SCALE_POINT"},
				     {OBS_SCALE_BICUBIC, "OBS_SCALE_BICUBIC"},
				     {OBS_SCALE_BILINEAR, "OBS_SCALE_BILINEAR"},
				     {OBS_SCALE_LANCZOS, "OBS_SCALE_LANCZOS"},
				     {OBS_SCALE_AREA, "OBS_SCALE_AREA"},
			     })

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(media_frames_per_second, numerator,
				   denominator)

namespace GoLiveApi {
using std::string;
using std::optional;
using json = nlohmann::json;

struct Client {
	string name = "obs-studio";
	string version;
	std::vector<string> supported_codecs;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(Client, name, version, supported_codecs)
};

struct Cpu {
	int32_t physical_cores;
	int32_t logical_cores;
	optional<uint32_t> speed;
	optional<string> name;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(Cpu, physical_cores, logical_cores,
				       speed, name)
};

struct Memory {
	uint64_t total;
	uint64_t free;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(Memory, total, free)
};

struct Gpu {
	string model;
	uint32_t vendor_id;
	uint32_t device_id;
	uint64_t dedicated_video_memory;
	uint64_t shared_system_memory;
	optional<string> driver_version;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(Gpu, model, vendor_id, device_id,
				       dedicated_video_memory,
				       shared_system_memory, driver_version)
};

struct GamingFeatures {
	optional<bool> game_bar_enabled;
	optional<bool> game_dvr_allowed;
	optional<bool> game_dvr_enabled;
	optional<bool> game_dvr_bg_recording;
	optional<bool> game_mode_enabled;
	optional<bool> hags_enabled;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(GamingFeatures, game_bar_enabled,
				       game_dvr_allowed, game_dvr_enabled,
				       game_dvr_bg_recording, game_mode_enabled,
				       hags_enabled)
};

struct System {
	string version;
	string name;
	int build;
	string release;
	int revision;
	int bits;
	bool arm;
	bool armEmulation;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(System, version, name, build, release,
				       revision, bits, arm, armEmulation)
};

struct ExtraView {
	string name;
	uint32_t canvas_width;
	uint32_t canvas_height;
	media_frames_per_second framerate;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(ExtraView, name, canvas_width,
				       canvas_height, framerate)
};

struct Capabilities {
	Cpu cpu;
	Memory memory;
	optional<GamingFeatures> gaming_features;
	System system;
	optional<std::vector<Gpu>> gpu;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(Capabilities, cpu, memory,
				       gaming_features, system, gpu)
};

struct Preferences {
	optional<uint64_t> maximum_aggregate_bitrate;
	optional<uint32_t> maximum_video_tracks;
	optional<std::vector<ExtraView>> extra_views;
	bool vod_track_audio;
	uint32_t width;
	uint32_t height;
	media_frames_per_second framerate;
	uint32_t canvas_width;
	uint32_t canvas_height;
	optional<uint32_t> composition_gpu_index;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(Preferences, maximum_aggregate_bitrate,
				       maximum_video_tracks, extra_views,
				       vod_track_audio, width, height,
				       framerate, canvas_width, canvas_height,
				       composition_gpu_index)
};

struct PostData {
	string service;
	string schema_version;
	string authentication;

	Client client;
	Capabilities capabilities;
	Preferences preferences;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(PostData, service, schema_version,
				       authentication, client, capabilities,
				       preferences)
};

// Config Response

struct Meta {
	string service;
	string schema_version;
	string config_id;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(Meta, service, schema_version, config_id)
};

enum struct StatusResult {
	Unknown,
	Success,
	Warning,
	Error,
};

NLOHMANN_JSON_SERIALIZE_ENUM(StatusResult,
			     {
				     {StatusResult::Unknown, nullptr},
				     {StatusResult::Success, "success"},
				     {StatusResult::Warning, "warning"},
				     {StatusResult::Error, "error"},
			     })

struct Status {
	StatusResult result = StatusResult::Unknown;
	optional<string> html_en_us;
	NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Status, result, html_en_us)
};

struct IngestEndpoint {
	string protocol;
	string url_template;
	optional<string> authentication;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(IngestEndpoint, protocol,
						    url_template,
						    authentication)
};

struct VideoEncoderConfiguration {
	string type;
	uint32_t width;
	uint32_t height;
	optional<media_frames_per_second> framerate;
	optional<obs_scale_type> gpu_scale_type;
	optional<string> view;
	optional<json> bitrate_interpolation_points;
	json settings;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(
		VideoEncoderConfiguration, type, width, height, framerate,
		gpu_scale_type, view, bitrate_interpolation_points, settings)
};

struct AudioEncoderConfiguration {
	string codec;
	uint32_t track_id;
	uint32_t channels;
	json settings;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(AudioEncoderConfiguration, codec,
				       track_id, channels, settings)
};

struct AudioConfigurations {
	std::vector<AudioEncoderConfiguration> live;
	optional<std::vector<AudioEncoderConfiguration>> vod;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(AudioConfigurations, live,
						    vod)
};

struct Config {
	Meta meta;
	optional<Status> status;
	std::vector<IngestEndpoint> ingest_endpoints;
	std::vector<VideoEncoderConfiguration> encoder_configurations;
	AudioConfigurations audio_configurations;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE_WITH_DEFAULT(Config, meta, status,
						    ingest_endpoints,
						    encoder_configurations,
						    audio_configurations)
};
} // namespace GoLiveApi
