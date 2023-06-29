
#include "berryessa-submitter.hpp"
#include "berryessa-every-minute.hpp"
#include "presentmon-csv-capture.hpp"

#include <QTimer>
#include <QRandomGenerator>
#include <QDateTime>

BerryessaEveryMinute::BerryessaEveryMinute(QObject *parent,
					   BerryessaSubmitter *berryessa)
	: QObject(parent),
	  berryessa_(berryessa),
	  presentmon_(this),
	  timer_(this),
	  startTime_(QDateTime::currentDateTimeUtc())
{

	connect(&timer_, &QTimer::timeout, this, &BerryessaEveryMinute::fire);

	timer_.setSingleShot(true);
	quint64 msecs = QRandomGenerator::global()->generate64() % 60000;
	blog(LOG_INFO, "BerryessaEveryMinute - first invocation in %d ms",
	     (int)msecs);
	timer_.start(msecs);
}

BerryessaEveryMinute::~BerryessaEveryMinute() {}

void BerryessaEveryMinute::fire()
{
	blog(LOG_INFO, "BerryessaEveryMinute::fire called");

	OBSDataAutoRelease event = obs_data_create();

	presentmon_.summarizeAndReset(event);

	berryessa_->submit("ivs_obs_stream_minute", event);

	// XXX after the first firing at a random [0.000, 60.000) time, try to fire
	// every 60 seconds after that correcting for drift
	timer_.start(60000);
}
