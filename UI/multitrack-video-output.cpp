#include "multitrack-video-output.hpp"

#include <util/dstr.hpp>
#include <util/platform.h>
#include <util/profiler.hpp>
#include <util/util.hpp>
#include <obs-frontend-api.h>
#include <obs-app.hpp>
#include <obs.hpp>
#include <remote-text.hpp>

#include "window-basic-main.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include <QAbstractButton>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QScopeGuard>
#include <QString>
#include <QThreadPool>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

#include <nlohmann/json.hpp>

#include "system-info.hpp"
#include "goliveapi-postdata.hpp"
#include "goliveapi-network.hpp"
#include "multitrack-video-error.hpp"
#include "qt-helpers.hpp"
#include "models/multitrack-video.hpp"

using json = nlohmann::json;

Qt::ConnectionType BlockingConnectionTypeFor(QObject *object)
{
	return object->thread() == QThread::currentThread()
		       ? Qt::DirectConnection
		       : Qt::BlockingQueuedConnection;
}

bool MultitrackVideoDeveloperModeEnabled()
{
	static bool developer_mode = [] {
		auto args = qApp->arguments();
		for (const auto &arg : args) {
			if (arg == "--enable-multitrack-video-dev") {
				return true;
			}
		}
		return false;
	}();
	return developer_mode;
}

static void add_always_bool(BerryessaSubmitter *berryessa, const char *name,
			    bool data)
{
	if (!berryessa)
		return;

	berryessa->setAlwaysBool(name, data);
}

static void add_always_string(BerryessaSubmitter *berryessa, const char *name,
			      const char *data)
{
	if (!berryessa)
		return;

	berryessa->setAlwaysString(name, data);
}

static OBSServiceAutoRelease
create_service(const GoLiveApi::Config &go_live_config,
	       const std::optional<std::string> &rtmp_url,
	       const QString &in_stream_key, std::optional<bool> use_rtmps)
{
	const char *url = nullptr;
	QString stream_key = in_stream_key;

	const auto &ingest_endpoints = go_live_config.ingest_endpoints;

	for (auto &endpoint : ingest_endpoints) {
		if (qstrnicmp("RTMP", endpoint.protocol.c_str(), 4))
			continue;

		if (use_rtmps.has_value() &&
		    *use_rtmps !=
			    (qstricmp("RTMPS", endpoint.protocol.c_str()) == 0))
			continue;

		url = endpoint.url_template.c_str();
		if (endpoint.authentication &&
		    !endpoint.authentication->empty()) {
			blog(LOG_INFO,
			     "Using stream key supplied by autoconfig");
			stream_key = QString::fromStdString(
				*endpoint.authentication);
		}
		break;
	}

	if (rtmp_url.has_value()) {
		// Despite being set by user, it was set to a ""
		if (rtmp_url->empty()) {
			throw MultitrackVideoError::warning(QTStr(
				"FailedToStartStream.NoCustomRTMPURLInSettings"));
		}

		url = rtmp_url->c_str();
		blog(LOG_INFO, "Using custom RTMP URL: '%s'", url);
	} else {
		if (!url) {
			blog(LOG_ERROR, "No RTMP URL in go live config");
			throw MultitrackVideoError::warning(
				QTStr("FailedToStartStream.NoRTMPURLInConfig"));
		}

		blog(LOG_INFO, "Using URL template: '%s'", url);
	}

	DStr str;
	dstr_cat(str, url);

	// dstr_find does not protect against null, and dstr_cat will
	// not initialize str if cat'ing with a null url
	if (!dstr_is_empty(str)) {
		auto found = dstr_find(str, "/{stream_key}");
		if (found)
			dstr_remove(str, found - str->array,
				    str->len - (found - str->array));
	}

	QUrl parsed_url{url};
	QUrlQuery parsed_query{parsed_url};

	if (!go_live_config.meta.config_id.empty()) {
		parsed_query.addQueryItem(
			"obsConfigId",
			QString::fromStdString(go_live_config.meta.config_id));
	}

	auto key_with_param = stream_key;
	if (!parsed_query.isEmpty())
		key_with_param += "?" + parsed_query.toString();

	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_string(settings, "server", str->array);
	obs_data_set_string(settings, "key",
			    key_with_param.toUtf8().constData());

	auto service = obs_service_create(
		"rtmp_custom", "multitrack video service", settings, nullptr);

	if (!service) {
		blog(LOG_WARNING, "Failed to create multitrack video service");
		throw MultitrackVideoError::warning(QTStr(
			"FailedToStartStream.FailedToCreateMultitrackVideoService"));
	}

	return service;
}

static void ensure_directory_exists(std::string &path)
{
	replace(path.begin(), path.end(), '\\', '/');

	size_t last = path.rfind('/');
	if (last == std::string::npos)
		return;

	std::string directory = path.substr(0, last);
	os_mkdirs(directory.c_str());
}

std::string GetOutputFilename(const std::string &path, const char *format)
{
	std::string strPath;
	strPath += path;

	char lastChar = strPath.back();
	if (lastChar != '/' && lastChar != '\\')
		strPath += "/";

	strPath += BPtr<char>{
		os_generate_formatted_filename("flv", false, format)};
	ensure_directory_exists(strPath);

	return strPath;
}

static OBSOutputAutoRelease create_output()
{
	OBSOutputAutoRelease output = obs_output_create(
		"rtmp_output", "rtmp multitrack video", nullptr, nullptr);

	if (!output) {
		blog(LOG_ERROR,
		     "Failed to create multitrack video rtmp output");
		throw MultitrackVideoError::warning(QTStr(
			"FailedToStartStream.FailedToCreateMultitrackVideoOutput"));
	}

	return output;
}

static OBSOutputAutoRelease create_recording_output(obs_data_t *settings)
{
	OBSOutputAutoRelease output = obs_output_create(
		"flv_output", "flv multitrack video", settings, nullptr);

	if (!output)
		blog(LOG_ERROR, "Failed to create multitrack video flv output");

	return output;
}

static void adjust_video_encoder_scaling(
	const char *view_name, const obs_video_info *ovi,
	const video_output_info *voi, obs_encoder_t *video_encoder,
	const GoLiveApi::VideoEncoderConfiguration &encoder_config,
	size_t encoder_index)
{
	auto requested_width = encoder_config.width;
	auto requested_height = encoder_config.height;

	auto width = ovi ? ovi->base_width : voi->width;
	auto height = ovi ? ovi->base_height : voi->height;

	if (width < requested_width || height < requested_height) {
		blog(LOG_WARNING,
		     "Requested resolution exceeds canvas/available resolution for encoder %zu: %" PRIu32
		     "x%" PRIu32 " > %" PRIu32 "x%" PRIu32 " (canvas: %s)",
		     encoder_index, requested_width, requested_height, width,
		     height, view_name ? view_name : "obs_base");
	}

	obs_encoder_set_scaled_size(video_encoder, requested_width,
				    requested_height);
	obs_encoder_set_gpu_scale_type(
		video_encoder,
		encoder_config.gpu_scale_type.value_or(OBS_SCALE_BICUBIC));
}

static uint32_t closest_divisor(const obs_video_info &ovi,
				const media_frames_per_second &target_fps)
{
	auto target = (uint64_t)target_fps.numerator * ovi.fps_den;
	auto source = (uint64_t)ovi.fps_num * target_fps.denominator;
	return std::max(1u, static_cast<uint32_t>(source / target));
}

static void adjust_encoder_frame_rate_divisor(
	const obs_video_info &ovi, obs_encoder_t *video_encoder,
	const GoLiveApi::VideoEncoderConfiguration &encoder_config,
	const size_t encoder_index)
{
	if (!encoder_config.framerate) {
		blog(LOG_WARNING, "`framerate` not specified for encoder %zu",
		     encoder_index);
		return;
	}
	media_frames_per_second requested_fps = *encoder_config.framerate;

	if (ovi.fps_num == requested_fps.numerator &&
	    ovi.fps_den == requested_fps.denominator)
		return;

	auto divisor = closest_divisor(ovi, requested_fps);
	if (divisor <= 1)
		return;

	blog(LOG_INFO, "Setting frame rate divisor to %u for encoder %zu",
	     divisor, encoder_index);
	obs_encoder_set_frame_rate_divisor(video_encoder, divisor);
}

static const std::vector<const char *> &get_available_encoders()
{
	// encoders are currently only registered during startup, so keeping
	// a static vector around shouldn't be a problem
	static std::vector<const char *> available_encoders = [] {
		std::vector<const char *> available_encoders;
		for (size_t i = 0;; i++) {
			const char *id = nullptr;
			if (!obs_enum_encoder_types(i, &id))
				break;
			available_encoders.push_back(id);
		}
		return available_encoders;
	}();
	return available_encoders;
}

static bool encoder_available(const char *type)
{
	auto &encoders = get_available_encoders();
	return std::find_if(std::begin(encoders), std::end(encoders),
			    [=](const char *encoder) {
				    return strcmp(type, encoder) == 0;
			    }) != std::end(encoders);
}

static OBSEncoderAutoRelease create_video_encoder(
	DStr &name_buffer, size_t encoder_index,
	const GoLiveApi::EncoderConfiguration<
		GoLiveApi::VideoEncoderConfiguration> &encoder_config,
	const std::map<std::string, video_t *> &extra_views)
{
	auto encoder_type = encoder_config.config.type.c_str();
	if (!encoder_available(encoder_type)) {
		blog(LOG_ERROR, "Encoder type '%s' not available",
		     encoder_type);
		throw MultitrackVideoError::warning(
			QTStr("FailedToStartStream.EncoderNotAvailable")
				.arg(encoder_type));
	}

	dstr_printf(name_buffer, "multitrack video video encoder %zu",
		    encoder_index);

	OBSDataAutoRelease encoder_settings =
		obs_data_create_from_json(encoder_config.data.dump().c_str());
	obs_data_set_bool(encoder_settings, "disable_scenecut", true);

	OBSEncoderAutoRelease video_encoder = obs_video_encoder_create(
		encoder_type, name_buffer, encoder_settings, nullptr);
	if (!video_encoder) {
		blog(LOG_ERROR, "failed to create video encoder '%s'",
		     name_buffer->array);
		throw MultitrackVideoError::warning(
			QTStr("FailedToStartStream.FailedToCreateVideoEncoder")
				.arg(name_buffer->array, encoder_type));
	}

	obs_video_info ovi_storage;
	if (!obs_get_video_info(&ovi_storage)) {
		blog(LOG_WARNING,
		     "Failed to get obs_video_info while creating encoder %zu",
		     encoder_index);
		throw MultitrackVideoError::warning(
			QTStr("FailedToStartStream.FailedToGetOBSVideoInfo")
				.arg(name_buffer->array, encoder_type));
	}
	obs_video_info *ovi = &ovi_storage;

	const char *view_name = nullptr;
	video_t *video = nullptr;
	if (encoder_config.config.view.has_value()) {
		view_name = encoder_config.config.view->c_str();
		auto it = extra_views.find(view_name);
		if (it != extra_views.end()) {
			video = it->second;
			ovi = nullptr;
		}
	}

	obs_encoder_set_video(video_encoder, video ? video : obs_get_video());

	auto voi = video ? video_output_get_info(video) : nullptr;
	adjust_video_encoder_scaling(view_name, ovi, voi, video_encoder,
				     encoder_config.config, encoder_index);
	adjust_encoder_frame_rate_divisor(ovi_storage, video_encoder,
					  encoder_config.config, encoder_index);

	return video_encoder;
}

static OBSEncoderAutoRelease create_audio_encoder(const char *name,
						  const char *audio_encoder_id,
						  uint32_t audio_bitrate,
						  size_t mixer_idx)
{
	OBSDataAutoRelease settings = obs_data_create();
	obs_data_set_int(settings, "bitrate", audio_bitrate);

	OBSEncoderAutoRelease audio_encoder = obs_audio_encoder_create(
		audio_encoder_id, name, settings, mixer_idx, nullptr);
	if (!audio_encoder) {
		blog(LOG_ERROR, "Failed to create audio encoder");
		throw MultitrackVideoError::warning(QTStr(
			"FailedToStartStream.FailedToCreateAudioEncoder"));
	}
	obs_encoder_set_audio(audio_encoder, obs_get_audio());
	return audio_encoder;
}

struct OBSOutputs {
	OBSOutputAutoRelease output, recording_output;
};

static OBSOutputs
SetupOBSOutput(obs_data_t *dump_stream_to_file_config,
	       const GoLiveApi::Config &go_live_config,
	       std::vector<OBSEncoderAutoRelease> &audio_encoders,
	       std::vector<OBSEncoderAutoRelease> &video_encoders,
	       const char *audio_encoder_id,
	       std::optional<size_t> vod_track_mixer,
	       const std::map<std::string, video_t *> &extra_views);
static void SetupSignalHandlers(bool recording, MultitrackVideoOutput *self,
				obs_output_t *output, OBSSignal &start,
				OBSSignal &stop, OBSSignal &deactivate);

struct ModuleHash {
	std::string Algorithm;
	std::string Hash;
	std::string Path;

	NLOHMANN_DEFINE_TYPE_INTRUSIVE(ModuleHash, Algorithm, Hash, Path)
};

struct HashMismatch {
	const char *file_name;
	const char *current_hash;
	std::string expected_hash;
};

std::optional<std::vector<HashMismatch>>
check_plugin_hash_mismatches(const char *text)
{
	using json = nlohmann::json;

	std::vector<HashMismatch> mismatches;

	try {
		auto data = json::parse(text);

		auto &hashes = data["/hashes"_json_pointer];
		std::optional<json::exception> exception;

		auto callback = [&](obs_module_t *module) {
			if (exception.has_value())
				return;

			auto file_name = obs_get_module_file_name(module);
			if (!file_name)
				return;

			auto hash = obs_get_module_hash_sha256(module);
			if (!hash)
				return;

			auto file_name_length = strlen(file_name);

			for (auto &entry : hashes)
				try {
					auto module_hash =
						entry.template get<ModuleHash>();

					if (qstrnicmp(module_hash.Algorithm
							      .c_str(),
						      "SHA256", 7) != 0)
						continue;

					if (module_hash.Path.size() <
					    file_name_length)
						continue;

					auto potential_file_name_offset =
						module_hash.Path.size() -
						file_name_length;

					auto potential_file_name_start =
						module_hash.Path.c_str() +
						potential_file_name_offset;
					if (qstrnicmp(potential_file_name_start,
						      file_name,
						      file_name_length) != 0)
						continue;

					if (qstrnicmp(module_hash.Hash.c_str(),
						      hash,
						      module_hash.Hash.size()) ==
					    0)
						return;

					mismatches.push_back(
						{file_name, hash,
						 module_hash.Hash});
					return;
				} catch (json::exception e) {
					exception.emplace(std::move(e));
				}
		};
		using callback_t = decltype(callback);

		obs_enum_modules(
			[](void *context, obs_module_t *module) {
				(*static_cast<callback_t *>(context))(module);
			},
			&callback);

		if (exception)
			throw *exception;
	} catch (const json::exception &exception) {
		blog(LOG_ERROR,
		     "check_plugin_hash_mismatches: Error while processing plugin integrity json (%d): %s",
		     exception.id, exception.what());
		return std::nullopt;
	}

	return {mismatches};
}

void MultitrackVideoOutput::PrepareStreaming(
	QWidget *parent, const char *service_name, obs_service_t *service,
	const std::optional<std::string> &rtmp_url, const QString &stream_key,
	const char *audio_encoder_id,
	std::optional<uint32_t> maximum_aggregate_bitrate,
	std::optional<uint32_t> maximum_video_tracks,
	std::optional<std::string> custom_config,
	obs_data_t *dump_stream_to_file_config,
	std::optional<size_t> vod_track_mixer, std::optional<bool> use_rtmps)
{
	if (!berryessa_) {
		QMetaObject::invokeMethod(
			parent,
			[&] {
				berryessa_ = std::make_unique<BerryessaSubmitter>(
					parent,
					"https://data.stats.live-video.net/");
			},
			BlockingConnectionTypeFor(parent));
	}

	if (berryessa_every_minute_ && berryessa_every_minute_->has_value()) {
		// destruct berryessa_every_minute on main thread
		QMetaObject::invokeMethod(
			parent, [bem = std::move(berryessa_every_minute_)] {});
	}

	{
		const std::lock_guard<std::mutex> current_lock{current_mutex};
		const std::lock_guard<std::mutex> current_stream_dump_lock{
			current_stream_dump_mutex};
		if (current || current_stream_dump) {
			blog(LOG_WARNING,
			     "Tried to prepare multitrack video output while it's already active");
			return;
		}
	}

	if (!berryessa_every_minute_) {
		berryessa_every_minute_ =
			std::make_shared<std::optional<BerryessaEveryMinute>>(
				std::nullopt);
	}

	std::optional<GoLiveApi::Config> go_live_config;
	std::optional<GoLiveApi::Config> custom;
	bool is_custom_config = custom_config.has_value();
	auto auto_config_url = MultitrackVideoAutoConfigURL(service);

	OBSDataAutoRelease service_settings = obs_service_get_settings(service);
	auto multitrack_video_name =
		QTStr("Basic.Settings.Stream.MultitrackVideoLabel");
	if (obs_data_has_user_value(service_settings,
				    "multitrack_video_name")) {
		multitrack_video_name = obs_data_get_string(
			service_settings, "multitrack_video_name");
	}

	auto auto_config_url_data = auto_config_url.toUtf8();

	DStr vod_track_info_storage;
	if (vod_track_mixer.has_value())
		dstr_printf(vod_track_info_storage, "Yes (mixer: %zu)",
			    vod_track_mixer.value());

	blog(LOG_INFO,
	     "Preparing enhanced broadcasting stream for:\n"
	     "    custom config:  %s\n"
	     "    config url:     %s\n"
	     "  settings:\n"
	     "    service:               %s\n"
	     "    max aggregate bitrate: %s (%" PRIu32 ")\n"
	     "    max video tracks:      %s (%" PRIu32 ")\n"
	     "    custom rtmp url:       %s ('%s')\n"
	     "    vod track:             %s",
	     is_custom_config ? "Yes" : "No",
	     !auto_config_url.isEmpty() ? auto_config_url_data.constData()
					: "(null)",
	     service_name,
	     maximum_aggregate_bitrate.has_value() ? "Set" : "Auto",
	     maximum_aggregate_bitrate.value_or(0),
	     maximum_video_tracks.has_value() ? "Set" : "Auto",
	     maximum_video_tracks.value_or(0),
	     rtmp_url.has_value() ? "Yes" : "No",
	     rtmp_url.has_value() ? rtmp_url->c_str() : "",
	     vod_track_info_storage->array ? vod_track_info_storage->array
					   : "No");

	for (auto &video_output : OBSBasic::Get()->multitrackVideoViews) {
		video_t *video = video_output.start_video(video_output.param);
		if (!video)
			continue;
		extra_views_[video_output.name] = video;
	}

	auto views_guard = qScopeGuard([this] { StopExtraViews(); });

	const bool custom_config_only =
		auto_config_url.isEmpty() &&
		MultitrackVideoDeveloperModeEnabled() &&
		custom_config.has_value() &&
		strcmp(obs_service_get_id(service), "rtmp_custom") == 0;

	if (!custom_config_only) {
		auto go_live_post = constructGoLivePost(
			stream_key, maximum_aggregate_bitrate,
			maximum_video_tracks, vod_track_mixer.has_value(),
			extra_views_);

		go_live_config = DownloadGoLiveConfig(parent, auto_config_url,
						      go_live_post,
						      multitrack_video_name);

		blog(LOG_INFO, "Enhanced broadcasting config_id: '%s'",
		     go_live_config->meta.config_id.c_str());
		if (berryessa_) {
			add_always_string(
				berryessa_.get(), "config_id",
				go_live_config->meta.config_id.c_str());
		}
	}

	auto berryessa_guard = qScopeGuard([&] {
		if (berryessa_)
			berryessa_->unsetAlways("config_id");
	});

	if (custom_config.has_value()) {
		GoLiveApi::Config parsed_custom;
		try {
			parsed_custom = nlohmann::json::parse(*custom_config);
		} catch (const nlohmann::json::exception &exception) {
			blog(LOG_WARNING, "Failed to parse custom config: %s",
			     exception.what());
			throw MultitrackVideoError::critical(QTStr(
				"FailedToStartStream.InvalidCustomConfig"));
		}

		// copy unique ID from go live request
		if (go_live_config.has_value()) {
			parsed_custom.meta.config_id =
				go_live_config->meta.config_id;
			blog(LOG_INFO,
			     "Using config_id from go live config with custom config: %s",
			     parsed_custom.meta.config_id.c_str());
		}

		nlohmann::json custom_data = parsed_custom;
		blog(LOG_INFO, "Using custom go live config: %s",
		     custom_data.dump(4).c_str());

		custom.emplace(std::move(parsed_custom));
	}

	add_always_bool(berryessa_.get(), "config_custom", is_custom_config);

	if (!go_live_config && !custom) {
		blog(LOG_ERROR,
		     "MultitrackVideoOutput: no config set, this should never happen");
		throw MultitrackVideoError::warning(
			QTStr("FailedToStartStream.NoConfig"));
	}

	const auto &output_config = custom ? *custom : *go_live_config;
	const auto &service_config = go_live_config ? *go_live_config : *custom;

	auto audio_encoders = std::vector<OBSEncoderAutoRelease>();
	auto video_encoders = std::vector<OBSEncoderAutoRelease>();
	auto outputs = SetupOBSOutput(dump_stream_to_file_config, output_config,
				      audio_encoders, video_encoders,
				      audio_encoder_id, vod_track_mixer,
				      extra_views_);
	auto output = std::move(outputs.output);
	auto recording_output = std::move(outputs.recording_output);
	if (!output)
		throw MultitrackVideoError::warning(
			QTStr("FailedToStartStream.FallbackToDefault")
				.arg(multitrack_video_name));

	auto multitrack_video_service =
		create_service(service_config, rtmp_url, stream_key, use_rtmps);
	if (!multitrack_video_service)
		throw MultitrackVideoError::warning(
			QTStr("FailedToStartStream.FallbackToDefault")
				.arg(multitrack_video_name));

	obs_output_set_service(output, multitrack_video_service);

	// Enable metrics delivery over SEI for multitrack live services
	obs_output_enable_bpm(output, true);

	OBSSignal start_streaming;
	OBSSignal stop_streaming;
	OBSSignal deactivate_stream;
	SetupSignalHandlers(false, this, output, start_streaming,
			    stop_streaming, deactivate_stream);

	if (dump_stream_to_file_config && recording_output) {
		OBSSignal start_recording;
		OBSSignal stop_recording;
		OBSSignal deactivate_recording;
		SetupSignalHandlers(true, this, recording_output,
				    start_recording, stop_recording,
				    deactivate_recording);

		decltype(video_encoders) recording_video_encoders;
		recording_video_encoders.reserve(video_encoders.size());
		for (auto &encoder : video_encoders) {
			recording_video_encoders.emplace_back(
				obs_encoder_get_ref(encoder));
		}

		decltype(audio_encoders) recording_audio_encoders;
		recording_audio_encoders.reserve(audio_encoders.size());
		for (auto &encoder : audio_encoders) {
			recording_audio_encoders.emplace_back(
				obs_encoder_get_ref(encoder));
		}

		{
			const std::lock_guard current_stream_dump_lock{
				current_stream_dump_mutex};
			current_stream_dump.emplace(OBSOutputObjects{
				std::move(recording_output),
				std::move(recording_video_encoders),
				std::move(recording_audio_encoders),
				nullptr,
				std::move(start_recording),
				std::move(stop_recording),
				std::move(deactivate_recording),
			});
		}
	}

	{
		const std::lock_guard current_lock{current_mutex};
		current.emplace(OBSOutputObjects{
			std::move(output),
			std::move(video_encoders),
			std::move(audio_encoders),
			std::move(multitrack_video_service),
			std::move(start_streaming),
			std::move(stop_streaming),
			std::move(deactivate_stream),
		});
	}

	berryessa_guard.dismiss();
	views_guard.dismiss();
}

void MultitrackVideoOutput::StopExtraViews()
{
	for (auto &view : extra_views_) {
		for (auto &v : OBSBasic::Get()->multitrackVideoViews)
			if (v.name == view.first)
				v.stop_video(view.second, v.param);
	}
	extra_views_.clear();
}

signal_handler_t *MultitrackVideoOutput::StreamingSignalHandler()
{
	const std::lock_guard current_lock{current_mutex};
	return current.has_value()
		       ? obs_output_get_signal_handler(current->output_)
		       : nullptr;
}

void MultitrackVideoOutput::StartedStreaming(QWidget *parent)
{
	OBSOutputAutoRelease dump_output;
	{
		const std::lock_guard current_stream_dump_lock{
			current_stream_dump_mutex};
		if (current_stream_dump && current_stream_dump->output_) {
			dump_output = obs_output_get_ref(
				current_stream_dump->output_);
		}
	}

	if (dump_output) {
		auto result = obs_output_start(dump_output);
		blog(LOG_INFO, "MultitrackVideoOutput: starting recording%s",
		     result ? "" : " failed");
	}

	if (berryessa_ && current) {
		std::vector<OBSEncoder> video_encoders;
		video_encoders.reserve(current->video_encoders_.size());
		for (const auto &encoder : current->video_encoders_) {
			video_encoders.emplace_back(encoder);
		}

		berryessa_every_minute_initializer_.setFuture(
			CreateFuture().then(
				QThreadPool::globalInstance(),
				[=, bem = berryessa_every_minute_,
				 berryessa = berryessa_.get(),
				 main_thread = QThread::currentThread()] {
					bem->emplace(parent, berryessa,
						     video_encoders)
						.moveToThread(main_thread);
				}));
	}
}

void MultitrackVideoOutput::StopStreaming()
{
	OBSOutputAutoRelease current_output;
	{
		const std::lock_guard current_lock{current_mutex};
		if (current && current->output_)
			current_output = obs_output_get_ref(current->output_);
	}
	if (current_output)
		obs_output_stop(current_output);

	OBSOutputAutoRelease dump_output;
	{
		const std::lock_guard current_stream_dump_lock{
			current_stream_dump_mutex};
		if (current_stream_dump && current_stream_dump->output_)
			dump_output = obs_output_get_ref(
				current_stream_dump->output_);
	}
	if (dump_output)
		obs_output_stop(dump_output);
}

bool MultitrackVideoOutput::HandleIncompatibleSettings(
	QWidget *parent, config_t *config, obs_service_t *service,
	bool &useDelay, bool &enableNewSocketLoop)
{
	QString incompatible_settings;
	QString where_to_disable;
	QString incompatible_settings_list;

	size_t num = 1;

	auto check_setting = [&](bool setting, const char *name,
				 const char *section) {
		if (!setting)
			return;

		incompatible_settings +=
			QString(" %1. %2\n").arg(num).arg(QTStr(name));

		where_to_disable +=
			QString(" %1. [%2 → %3 → %4]\n")
				.arg(num)
				.arg(QTStr("Settings"))
				.arg(QTStr("Basic.Settings.Advanced"))
				.arg(QTStr(section));

		incompatible_settings_list += QString("%1, ").arg(name);

		num += 1;
	};

	check_setting(useDelay, "Basic.Settings.Advanced.StreamDelay",
		      "Basic.Settings.Advanced.StreamDelay");
#ifdef _WIN32
	check_setting(enableNewSocketLoop,
		      "Basic.Settings.Advanced.Network.EnableNewSocketLoop",
		      "Basic.Settings.Advanced.Network");
#endif

	if (incompatible_settings.isEmpty())
		return true;

	OBSDataAutoRelease service_settings = obs_service_get_settings(service);

	QMessageBox mb(parent);
	mb.setIcon(QMessageBox::Critical);
	mb.setWindowTitle(QTStr("MultitrackVideo.IncompatibleSettings.Title"));
	mb.setText(QString(QTStr("MultitrackVideo.IncompatibleSettings.Text"))
			   .arg(obs_data_get_string(service_settings,
						    "multitrack_video_name"))
			   .arg(incompatible_settings)
			   .arg(where_to_disable));
	auto this_stream = mb.addButton(
		QTStr("MultitrackVideo.IncompatibleSettings.DisableAndStartStreaming"),
		QMessageBox::AcceptRole);
	auto all_streams = mb.addButton(
		QString(QTStr(
			"MultitrackVideo.IncompatibleSettings.UpdateAndStartStreaming")),
		QMessageBox::AcceptRole);
	mb.setStandardButtons(QMessageBox::StandardButton::Cancel);

	mb.exec();

	const char *action = "cancel";
	if (mb.clickedButton() == this_stream) {
		action = "DisableAndStartStreaming";
	} else if (mb.clickedButton() == all_streams) {
		action = "UpdateAndStartStreaming";
	}

	blog(LOG_INFO,
	     "MultitrackVideoOutput: attempted to start stream with incompatible"
	     "settings (%s); action taken: %s",
	     incompatible_settings_list.toUtf8().constData(), action);

	if (mb.clickedButton() == this_stream ||
	    mb.clickedButton() == all_streams) {
		useDelay = false;
		enableNewSocketLoop = false;

		if (mb.clickedButton() == all_streams) {
			config_set_bool(config, "Output", "DelayEnable", false);
#ifdef _WIN32
			config_set_bool(config, "Output", "NewSocketLoopEnable",
					false);
#endif
			config_set_bool(config, "Output", "DynamicBitrate",
					false);
		}

		return true;
	}

	MultitrackVideoOutput::ReleaseOnMainThread(take_current());
	MultitrackVideoOutput::ReleaseOnMainThread(take_current_stream_dump());

	return false;
}

static bool
create_video_encoders(const GoLiveApi::Config &go_live_config,
		      std::vector<OBSEncoderAutoRelease> &video_encoders,
		      obs_output_t *output, obs_output_t *recording_output,
		      const std::map<std::string, video_t *> &extra_views,
		      json &bitrate_interpolation_array)
{
	DStr video_encoder_name_buffer;
	obs_encoder_t *first_encoder = nullptr;
	if (go_live_config.encoder_configurations.empty()) {
		blog(LOG_WARNING,
		     "MultitrackVideoOutput: Missing video encoder configurations");
		throw MultitrackVideoError::warning(
			QTStr("FailedToStartStream.MissingEncoderConfigs"));
	}

	for (size_t i = 0; i < go_live_config.encoder_configurations.size();
	     i++) {
		auto encoder = create_video_encoder(
			video_encoder_name_buffer, i,
			go_live_config.encoder_configurations[i], extra_views);
		if (!encoder)
			return false;

		if (!first_encoder)
			first_encoder = encoder;
		else
			obs_encoder_group_keyframe_aligned_encoders(
				first_encoder, encoder);

		obs_output_set_video_encoder2(output, encoder, i);
		if (recording_output)
			obs_output_set_video_encoder2(recording_output, encoder,
						      i);
		video_encoders.emplace_back(std::move(encoder));

		auto &data = go_live_config.encoder_configurations[i]
				     .config.bitrate_interpolation_points;
		if (data.has_value())
			bitrate_interpolation_array.push_back(*data);
		else
			bitrate_interpolation_array.push_back(json::array());
	}

	return true;
}

static void
create_audio_encoders(const GoLiveApi::Config &go_live_config,
		      std::vector<OBSEncoderAutoRelease> &audio_encoders,
		      obs_output_t *output, obs_output_t *recording_output,
		      const char *audio_encoder_id,
		      std::optional<size_t> vod_track_mixer)
{
	speaker_layout speakers = SPEAKERS_UNKNOWN;
	obs_audio_info oai = {};
	if (obs_get_audio_info(&oai))
		speakers = oai.speakers;

	auto sanitize_audio_channels = [&](obs_encoder_t *encoder,
					   uint32_t channels) {
		speaker_layout target_speakers = SPEAKERS_UNKNOWN;
		for (size_t i = 0; i <= (size_t)SPEAKERS_7POINT1; i++) {
			if (get_audio_channels((speaker_layout)i) != channels)
				continue;

			target_speakers = (speaker_layout)i;
			break;
		}
		if (target_speakers == SPEAKERS_UNKNOWN) {
			blog(LOG_WARNING,
			     "MultitrackVideoOutput: Could not find "
			     "speaker layout for %" PRIu32 "channels "
			     "while configuring encoder '%s'",
			     channels, obs_encoder_get_name(encoder));
			return;
		}
		if (speakers != SPEAKERS_UNKNOWN &&
		    channels > get_audio_channels(speakers))
			return;

		obs_encoder_set_speaker_layout(encoder, target_speakers);
		blog(LOG_INFO,
		     "MultitrackVideoOutput: settings encoder '%s' "
		     "to %" PRIu32 " channels ",
		     obs_encoder_get_name(encoder), channels);
	};

	using encoder_configs_type =
		decltype(go_live_config.audio_configurations.live);
	DStr encoder_name_buffer;
	size_t output_encoder_index = 0;

	auto create_encoders = [&](const char *name_prefix,
				   const encoder_configs_type &configs,
				   size_t mixer_idx) {
		if (configs.empty()) {
			blog(LOG_WARNING,
			     "MultitrackVideoOutput: Missing audio encoder configurations (for '%s')",
			     name_prefix);
			throw MultitrackVideoError::warning(QTStr(
				"FailedToStartStream.MissingEncoderConfigs"));
		}

		for (size_t i = 0; i < configs.size(); i++) {
			dstr_printf(encoder_name_buffer, "%s %zu", name_prefix,
				    i);
			OBSEncoderAutoRelease audio_encoder =
				create_audio_encoder(encoder_name_buffer->array,
						     audio_encoder_id,
						     configs[i].config.bitrate,
						     mixer_idx);

			sanitize_audio_channels(audio_encoder,
						configs[i].config.channels);

			obs_output_set_audio_encoder(output, audio_encoder,
						     output_encoder_index);
			if (recording_output)
				obs_output_set_audio_encoder(
					recording_output, audio_encoder,
					output_encoder_index);
			output_encoder_index += 1;
			audio_encoders.emplace_back(std::move(audio_encoder));
		}
	};

	create_encoders("multitrack video live audio",
			go_live_config.audio_configurations.live, 0);

	if (!vod_track_mixer.has_value())
		return;

	create_encoders("multitrack video vod audio",
			go_live_config.audio_configurations.vod,
			*vod_track_mixer);

	return;
}

static OBSOutputs
SetupOBSOutput(obs_data_t *dump_stream_to_file_config,
	       const GoLiveApi::Config &go_live_config,
	       std::vector<OBSEncoderAutoRelease> &audio_encoders,
	       std::vector<OBSEncoderAutoRelease> &video_encoders,
	       const char *audio_encoder_id,
	       std::optional<size_t> vod_track_mixer,
	       const std::map<std::string, video_t *> &extra_views)
{

	auto output = create_output();
	OBSOutputAutoRelease recording_output;
	if (dump_stream_to_file_config)
		recording_output =
			create_recording_output(dump_stream_to_file_config);

	json bitrate_interpolation_array = json::array();
	if (!create_video_encoders(go_live_config, video_encoders, output,
				   recording_output, extra_views,
				   bitrate_interpolation_array))
		return {nullptr, nullptr};

	OBSDataAutoRelease settings = obs_output_get_settings(output);
	obs_data_set_string(settings, "interpolation_table_data",
			    bitrate_interpolation_array.dump().c_str());
	obs_output_update(output, settings);

	create_audio_encoders(go_live_config, audio_encoders, output,
			      recording_output, audio_encoder_id,
			      vod_track_mixer);

	return {std::move(output), std::move(recording_output)};
}

void SetupSignalHandlers(bool recording, MultitrackVideoOutput *self,
			 obs_output_t *output, OBSSignal &start,
			 OBSSignal &stop, OBSSignal &deactivate)
{
	auto handler = obs_output_get_signal_handler(output);

	if (recording)
		start.Connect(handler, "start", RecordingStartHandler, self);

	stop.Connect(handler, "stop",
		     !recording ? StreamStopHandler : RecordingStopHandler,
		     self);

	deactivate.Connect(handler, "deactivate",
			   !recording ? StreamDeactivateHandler
				      : RecordingDeactivateHandler,
			   self);
}

std::optional<MultitrackVideoOutput::OBSOutputObjects>
MultitrackVideoOutput::take_current()
{
	const std::lock_guard<std::mutex> current_lock{current_mutex};
	auto val = std::move(current);
	current.reset();
	return val;
}

std::optional<MultitrackVideoOutput::OBSOutputObjects>
MultitrackVideoOutput::take_current_stream_dump()
{
	const std::lock_guard<std::mutex> current_stream_dump_lock{
		current_stream_dump_mutex};
	auto val = std::move(current_stream_dump);
	current_stream_dump.reset();
	return val;
}

void MultitrackVideoOutput::ReleaseOnMainThread(
	std::optional<OBSOutputObjects> objects)
{
	if (!objects.has_value())
		return;

	QMetaObject::invokeMethod(
		QApplication::instance()->thread(),
		[objects = std::move(objects)] {}, Qt::QueuedConnection);
}

void StreamStopHandler(void *arg, calldata_t *params)
{
	auto self = static_cast<MultitrackVideoOutput *>(arg);
	self->StopExtraViews();

	OBSOutputAutoRelease stream_dump_output;
	{
		const std::lock_guard<std::mutex> current_stream_dump_lock{
			self->current_stream_dump_mutex};
		if (self->current_stream_dump &&
		    self->current_stream_dump->output_)
			stream_dump_output = obs_output_get_ref(
				self->current_stream_dump->output_);
	}
	if (stream_dump_output)
		obs_output_stop(stream_dump_output);

	if (obs_output_active(static_cast<obs_output_t *>(
		    calldata_ptr(params, "output"))))
		return;

	auto object_releaser = self->take_current();
	QMetaObject::invokeMethod(
		QApplication::instance()->thread(),
		[berryessa = QPointer{self->berryessa_.get()},
		 bem = std::move(self->berryessa_every_minute_),
		 object_releaser = std::move(object_releaser)] {
			if (berryessa) {
				berryessa->unsetAlways("config_id");
				berryessa->unsetAlways(
					"stream_attempt_start_time");
			}
		},
		Qt::QueuedConnection);

	self->berryessa_every_minute_ =
		std::make_shared<std::optional<BerryessaEveryMinute>>(
			std::nullopt);
}

void StreamDeactivateHandler(void *arg, calldata_t *params)
{
	auto self = static_cast<MultitrackVideoOutput *>(arg);

	if (obs_output_reconnecting(static_cast<obs_output_t *>(
		    calldata_ptr(params, "output"))))
		return;

	MultitrackVideoOutput::ReleaseOnMainThread(self->take_current());
}

void RecordingStartHandler(void * /* arg */, calldata_t * /* data */)
{
	blog(LOG_INFO, "MultitrackVideoOutput: recording started");
}

void RecordingStopHandler(void *arg, calldata_t *params)
{
	auto self = static_cast<MultitrackVideoOutput *>(arg);
	blog(LOG_INFO, "MultitrackVideoOutput: recording stopped");

	if (obs_output_active(static_cast<obs_output_t *>(
		    calldata_ptr(params, "output"))))
		return;

	MultitrackVideoOutput::ReleaseOnMainThread(
		self->take_current_stream_dump());
}

void RecordingDeactivateHandler(void *arg, calldata_t * /*data*/)
{
	auto self = static_cast<MultitrackVideoOutput *>(arg);
	MultitrackVideoOutput::ReleaseOnMainThread(
		self->take_current_stream_dump());
}
