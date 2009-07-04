// $Id$

#ifndef MSXREALTIME_HH
#define MSXREALTIME_HH

#include "Schedulable.hh"
#include "EventListener.hh"
#include "Observer.hh"
#include "EmuTime.hh"

namespace openmsx {

class MSXMotherBoard;
class GlobalSettings;
class EventDistributor;
class IntegerSetting;
class BooleanSetting;
class ThrottleManager;
class Setting;

class RealTime : private Schedulable, private EventListener,
                 private Observer<Setting>, private Observer<ThrottleManager>
{
public:
	explicit RealTime(
		MSXMotherBoard& motherBoard, GlobalSettings& globalSettings);
	~RealTime();

	/** Convert EmuTime to RealTime.
	  */
	double getRealDuration(EmuTime::param time1, EmuTime::param time2);

	/** Convert RealTime to EmuTime.
	  */
	EmuDuration getEmuDuration(double realDur);

	/** Check that there is enough real time left before we reach as certain
	  * point in emulated time.
	  * @param us Real time duration is micro seconds.
	  * @param time Point in emulated time.
	  */
	bool timeLeft(unsigned long long us, EmuTime::param time);

	void resync();

private:
	/** Synchronize EmuTime with RealTime.
	  * @param time The current emulation time.
	  * @param allowSleep Is this method allowed to sleep, typically the
	  *                   result of a previous call to timeLeft() is passed.
	  */
	void sync(EmuTime::param time, bool allowSleep);

	// Schedulable
	virtual void executeUntil(EmuTime::param time, int userData);
	virtual const std::string& schedName() const;

	// EventListener
	virtual bool signalEvent(shared_ptr<const Event> event);

	// Observer<Setting>
	void update(const Setting& setting);
	// Observer<ThrottleManager>
	void update(const ThrottleManager& throttleManager);

	void internalSync(EmuTime::param time, bool allowSleep);

	MSXMotherBoard& motherBoard;
	EventDistributor& eventDistributor;
	ThrottleManager& throttleManager;
	IntegerSetting& speedSetting;
	BooleanSetting& pauseSetting;
	BooleanSetting& powerSetting;

	unsigned long long idealRealTime;
	EmuTime emuTime;
	double sleepAdjust;
};

} // namespace openmsx

#endif
