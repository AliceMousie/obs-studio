
#include "berryessa-submitter.hpp"

#include "copy-from-obs/remote-text.hpp"

BerryessaSubmitter::BerryessaSubmitter(QObject *parent, QString url)
	: QObject(parent), url_(url)
{
	this->alwaysProperties_ = obs_data_create();
}

BerryessaSubmitter::~BerryessaSubmitter() {}

void BerryessaSubmitter::submit(QString eventName, obs_data_t *properties)
{
	// overlay the supplied properties over a copy of the always properties
	//   (so if there's a conflict, supplied property wins)
	OBSDataAutoRelease newProperties = obs_data_create();
	obs_data_apply(newProperties, this->alwaysProperties_);
	obs_data_apply(newProperties, properties);

	// create {Name:, Properties:} object that Berryessa expects
	OBSDataAutoRelease toplevel = obs_data_create();
	obs_data_set_string(toplevel, "event", eventName.toUtf8());
	obs_data_set_obj(toplevel, "properties", newProperties);

	// XXX do the whole async worker thread thing
	std::vector<obs_data_t *> tmp;
	tmp.push_back(toplevel);
	auto error = syncSubmitReturningError(tmp);

	// not until we have async / queueing :)
	//if (error) {
	//  submit("ivs_obs_http_client_error", error);
	//}
}

OBSDataAutoRelease BerryessaSubmitter::syncSubmitReturningError(
	const std::vector<obs_data_t *> &items)
{
	// Berryessa documentation:
	// https://docs.google.com/document/d/1dB1fOgGQxu05ljqVVoX1jcjImzlcwcm9QKFZ2IDeuo0/edit#heading=h.yjke1ko59g7n

	// Build up JSON, releasing supplied obs_data_t*'s as we go
	QByteArray postJson;
	for (obs_data_t *it : items) {
		blog(LOG_INFO, "Berryessa: %s", obs_data_get_json(it));
		postJson += postJson.isEmpty() ? "[" : ",";
		postJson += obs_data_get_json(it);
	}
	postJson += "]";

	// base64 encoding for HTTP post
	QByteArray postEncoded("data=");
	postEncoded += postJson.toBase64();

	// http post to berryessa
	std::vector<std::string> headers;
	headers.push_back("User-Agent: obs-simulcast-plugin-tech-preview");
	headers.push_back(
		"Content-Type: application/x-www-form-urlencoded; charset=UTF-8");

	std::string httpResponse, httpError;
	long httpResponseCode;

	bool ok = GetRemoteFile(
		url_.toUtf8(), httpResponse, httpError, // out params
		&httpResponseCode,
		nullptr, // out params (response code and content type)
		"POST", postEncoded.constData(), headers,
		nullptr, // signature
		5);      // timeout in seconds

	// XXX parse response from berryessa, check response code?

	// log and return http error information, if any
	OBSDataAutoRelease error = nullptr;
	if (!ok) {
		error = obs_data_create();
		obs_data_set_string(error, "url", url_.toUtf8());
		obs_data_set_string(error, "error", httpError.c_str());
		obs_data_set_int(error, "response_code", httpResponseCode);
		blog(LOG_WARNING,
		     "Could not submit %lld bytes to metrics backend: %s",
		     postEncoded.size(), obs_data_get_json(error));
	}
	return error;
}

void BerryessaSubmitter::setAlwaysString(QString propertyKey,
					 QString propertyValue)
{
	obs_data_set_string(this->alwaysProperties_, propertyKey.toUtf8(),
			    propertyValue.toUtf8());
}

void BerryessaSubmitter::unsetAlways(QString propertyKey)
{
	obs_data_unset_user_value(this->alwaysProperties_,
				  propertyKey.toUtf8());
}
