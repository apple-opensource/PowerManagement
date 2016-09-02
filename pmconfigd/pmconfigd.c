/*
 * Copyright (c) 2001 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 * 
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * Copyright (c) 2001 Apple Computer, Inc.  All rights reserved. 
 *
 * HISTORY
 *
 * 18-Dec-01 ebold created
 *
 */
 
#include <CoreFoundation/CoreFoundation.h>

#include <SystemConfiguration/SystemConfiguration.h>
#include <SystemConfiguration/SCValidation.h>
#include <SystemConfiguration/SCPreferencesPrivate.h>
#include <SystemConfiguration/SCDynamicStorePrivate.h>
#include <SystemConfiguration/SCDynamicStoreCopySpecificPrivate.h>
#include <SystemConfiguration/SCDPlugin.h>

#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/pwr_mgt/IOPMLibPrivate.h>
#include <IOKit/IOKitKeysPrivate.h>
#include <IOKit/IOMessage.h>

#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#include <syslog.h>
#include <unistd.h>

#include "PMSettings.h"
#include "PSLowPower.h"
#include "BatteryTimeRemaining.h"
#include "AutoWakeScheduler.h"
#include "RepeatingAutoWake.h"
#include "PrivateLib.h"

#define kIOPMAppName		"Power Management configd plugin"
#define kIOPMPrefsPath		"com.apple.PowerManagement.xml"

#define kApplePMUUCMagicCookie      0x0101BEEF

// Global keys
static CFStringRef          EnergyPrefsKey = NULL;
static CFStringRef          AutoWakePrefsKey = NULL;
static CFStringRef          ConsoleUserKey = NULL;
static SCDynamicStoreRef    energyDS = NULL;

static io_service_t         gIOResourceService = 0;
static io_connect_t         _pm_ack_port = 0;


static void tellSMU_GMTOffset(void);


/* PMUInterestNotification
 *
 * Receives and distributes messages from the PMU driver
 * These include legacy AutoWake requests and battery change notifications.
 */
static void 
PMUInterestNotification(void *refcon, io_service_t service, natural_t messageType, void *arg)
{    
    // Tell the AutoWake handler
    if((kIOPMUMessageLegacyAutoWake == messageType) ||
       (kIOPMUMessageLegacyAutoPower == messageType) )
        AutoWakePMUInterestNotification(messageType, (UInt32)arg);
}

/* RootDomainInterestNotification
 *
 * Receives and distributes messages from the IOPMrootDomain
 */
static void 
RootDomainInterestNotification(void *refcon, io_service_t service, natural_t messageType, void *arg)
{
    CFArrayRef          battery_info;

    // Tell battery calculation code that battery status has changed
    if(kIOPMMessageBatteryStatusHasChanged == messageType)
    {
        // get battery info
        battery_info = isA_CFArray(_copyBatteryInfo());
        if(!battery_info) return;

        // Pass control over to PMSettings
        PMSettingsBatteriesHaveChanged(battery_info);
        // Pass control over to PMUBattery for battery calculation
        BatteryTimeRemainingBatteriesHaveChanged(battery_info);
        
        CFRelease(battery_info);
    }
}

/* SleepWakeCallback
 * 
 * Receives notifications on system sleep and system wake.
 */
static void
SleepWakeCallback(void * port,io_service_t y,natural_t messageType,void * messageArgument)
{
    // Notify BatteryTimeRemaining
    BatteryTimeRemainingSleepWakeNotification(messageType);

    // Notify PMSettings
    PMSettingsSleepWakeNotification(messageType);
    
    // Notify AutoWake
    AutoWakeSleepWakeNotification(messageType);
    RepeatingAutoWakeSleepWakeNotification(messageType);

    switch ( messageType ) {
    case kIOMessageSystemWillSleep:
        tellSMU_GMTOffset(); // tell SMU what our timezone offset is
    case kIOMessageCanSystemSleep:
        IOAllowPowerChange(_pm_ack_port, (long)messageArgument);
        break;
        
    case kIOMessageSystemHasPoweredOn:
        break;
    }
}

/* ESPrefsHaveChanged
 *
 * Is the handler that configd calls when someone "applies" new Energy Saver
 * Preferences. Since the preferences have probably changed, we re-read them
 * from disk and transmit the new settings to the kernel.
 */
static void 
ESPrefsHaveChanged(SCDynamicStoreRef store, CFArrayRef changedKeys, void *info) 
{
    CFRange   key_range = CFRangeMake(0, CFArrayGetCount(changedKeys));

    if(CFArrayContainsValue(changedKeys, key_range, EnergyPrefsKey))
    {
        // Tell PMSettings that the prefs file has changed
        PMSettingsPrefsHaveChanged();
        PSLowPowerPrefsHaveChanged();
    }

    if(CFArrayContainsValue(changedKeys, key_range, AutoWakePrefsKey))
    {
        // Tell AutoWake that the prefs file has changed
        AutoWakePrefsHaveChanged();
        RepeatingAutoWakePrefsHaveChanged();
    }

    if(CFArrayContainsValue(changedKeys, key_range, ConsoleUserKey))
    {
	CFArrayRef sessionList = SCDynamicStoreCopyConsoleInformation(energyDS);
	if (!sessionList)
	    sessionList = CFArrayCreate(kCFAllocatorDefault, NULL, 0, &kCFTypeArrayCallBacks);

	if (sessionList)
	{
	    IORegistryEntrySetCFProperty(gIOResourceService, CFSTR(kIOConsoleUsersKey), sessionList);
	    CFRelease(sessionList);
	}
    }

    return;
}


/* PowerSourcesHaveChanged
 *
 * Is the handler that gets notified when power source (battery or UPS)
 * state changes. We might respond to this by posting a user notification
 * or performing emergency sleep/shutdown.
 */
extern void
PowerSourcesHaveChanged(void *info) 
{
    CFTypeRef			ps_blob;
    
    ps_blob = isA_CFDictionary(IOPSCopyPowerSourcesInfo());
    if(!ps_blob) return;
    
    // Notifiy PSLowPower of power sources change
    PSLowPowerPSChange(ps_blob);
    
    // Notify PMSettings
    PMSettingsPSChange(ps_blob);
    
    CFRelease(ps_blob);
}


/* tellSMU_GMTOffset
 *
 * Tell the SMU what the seconds offset from GMT is.
 * Why does power management care which timezone we're in?
 * We don't, really. The SMU firmware needs to know for
 * a feature which shall remain nameless. Timezone info
 * is really only conveniently accessible from up here in 
 * user space, so we just keep track of it and tell PMU/SMU
 * whenever it changes. And this PM plugin was a vaguely
 * convenient place for this code to live.
 */
static void 
tellSMU_GMTOffset(void)
{
    static io_registry_entry_t  smuRegEntry = MACH_PORT_NULL;
    static io_connect_t         smuConnect = MACH_PORT_NULL;
    CFTimeZoneRef               tzr = NULL;
    CFDataRef                   d = NULL;
    int                         secondsOffset = 0;

    if(!systemHasSMU()) return;
    
    if(MACH_PORT_NULL == smuRegEntry)
    {
        // Find SMU node
        smuRegEntry = (io_registry_entry_t)IOServiceGetMatchingService(0,
                                    IOServiceNameMatching("AppleSMU"));
        if(MACH_PORT_NULL == smuRegEntry) {
            // Error - unable to locate SMU
            return;
        }
        // Open up SMU's User Client
        IOServiceOpen(smuRegEntry, mach_task_self(), kApplePMUUCMagicCookie, &smuConnect);
    }

    CFTimeZoneResetSystem();
    tzr = CFTimeZoneCopySystem();
    if(!tzr) goto exit;

    secondsOffset = (int)CFTimeZoneGetSecondsFromGMT(tzr, CFAbsoluteTimeGetCurrent());
    d = CFDataCreate(0, &secondsOffset, sizeof(int));
    if(!d) goto exit;

    IOConnectSetCFProperty(smuConnect, CFSTR("TimeZoneOffsetSeconds"), d);

exit:
    if(tzr) CFRelease(tzr);
    if(d) CFRelease(d);
}

/* displayPowerStateChange
 *
 * displayPowerStateChange gets notified when the display changes power state.
 * Power state changes look like this:
 * (1) Full power -> dim
 * (2) dim -> display sleep
 * (3) display sleep -> display sleep
 * 
 * We're interested in state transition 2. When that occurs on an SMU system
 * we'll tell the SMU what the system clock's offset from GMT is.
 */
static void 
displayPowerStateChange(void *ref, io_service_t service, natural_t messageType, void *arg)
{
    static      int level = 0;
    switch (messageType)
    {
        case kIOMessageDeviceWillPowerOff:
            level++;
            if(2 == level) {
                // Display is transition from dim to full sleep.
                tellSMU_GMTOffset();            
            }
            break;
            
        case kIOMessageDeviceHasPoweredOn:
            level = 0;
            break;
    }            
}


/* initializeESPrefsDynamicStore
 *
 * Registers a handler that configd calls when someone changes com.apple.PowerManagement.xml
 */
static void
initializeESPrefsDynamicStore(void)
{
    CFRunLoopSourceRef 		CFrls;
    
    energyDS = SCDynamicStoreCreate(NULL, CFSTR(kIOPMAppName), &ESPrefsHaveChanged, NULL);

    // Setup notification for changes in Energy Saver prefences
    EnergyPrefsKey = SCDynamicStoreKeyCreatePreferences(NULL, CFSTR(kIOPMPrefsPath), kSCPreferencesKeyApply);
    if(EnergyPrefsKey) 
        SCDynamicStoreAddWatchedKey(energyDS, EnergyPrefsKey, FALSE);

    // Setup notification for changes in AutoWake prefences
    AutoWakePrefsKey = SCDynamicStoreKeyCreatePreferences(NULL, CFSTR(kIOPMAutoWakePrefsPath), kSCPreferencesKeyCommit);
    if(AutoWakePrefsKey) 
        SCDynamicStoreAddWatchedKey(energyDS, AutoWakePrefsKey, FALSE);

    gIOResourceService = IORegistryEntryFromPath(NULL, kIOServicePlane ":/" kIOResourcesClass);
    ConsoleUserKey = SCDynamicStoreKeyCreateConsoleUser( NULL /* CFAllocator */ );
    if(ConsoleUserKey && gIOResourceService) 
        SCDynamicStoreAddWatchedKey(energyDS, ConsoleUserKey, FALSE);

    // Create and add RunLoopSource
    CFrls = SCDynamicStoreCreateRunLoopSource(NULL, energyDS, 0);
    if(CFrls) {
        CFRunLoopAddSource(CFRunLoopGetCurrent(), CFrls, kCFRunLoopDefaultMode);    
        CFRelease(CFrls);
    }

    return;
}

/* initializePowerSourceChanges
 *
 * Registers a handler that gets called on power source (battery or UPS) changes
 */
static void
initializePowerSourceChangeNotification(void)
{
    CFRunLoopSourceRef 		CFrls;
        
    // Create and add RunLoopSource
    CFrls = IOPSNotificationCreateRunLoopSource(PowerSourcesHaveChanged, NULL);
    if(CFrls) {
        CFRunLoopAddSource(CFRunLoopGetCurrent(), CFrls, kCFRunLoopDefaultMode);    
        CFRelease(CFrls);
    }
}

/* initializeInteresteNotifications
 *
 * Sets up the notification of general interest from the PMU & RootDomain
 */
static void
initializeInterestNotifications()
{
    IONotificationPortRef       notify_port = 0;
    IONotificationPortRef       r_notify_port = 0;
    io_object_t                 notification_ref = 0;
    io_service_t                pmu_service_ref = 0;
    io_service_t                root_domain_ref = 0;
    CFRunLoopSourceRef          rlser = 0;
    CFRunLoopSourceRef          r_rlser = 0;
    IOReturn                    ret;

    // PMU
    pmu_service_ref = IOServiceGetMatchingService(0, IOServiceNameMatching("ApplePMU"));
    if(!pmu_service_ref) goto root_domain;

    notify_port = IONotificationPortCreate(0);
    ret = IOServiceAddInterestNotification(notify_port, pmu_service_ref, 
                                kIOGeneralInterest, PMUInterestNotification,
                                0, &notification_ref);
    if(kIOReturnSuccess != ret) goto root_domain;

    rlser = IONotificationPortGetRunLoopSource(notify_port);
    if(!rlser) goto root_domain;

    CFRunLoopAddSource(CFRunLoopGetCurrent(), rlser, kCFRunLoopDefaultMode);
    
    
    // ROOT_DOMAIN
root_domain:
    root_domain_ref = IOServiceGetMatchingService(0, IOServiceNameMatching("IOPMrootDomain"));
    if(!root_domain_ref) goto finish;

    r_notify_port = IONotificationPortCreate(0);
    ret = IOServiceAddInterestNotification(r_notify_port, root_domain_ref, 
                                kIOGeneralInterest, RootDomainInterestNotification,
                                0, &notification_ref);
    if(kIOReturnSuccess != ret) goto finish;

    r_rlser = IONotificationPortGetRunLoopSource(r_notify_port);
    if(!r_rlser) goto finish;

    CFRunLoopAddSource(CFRunLoopGetCurrent(), r_rlser, kCFRunLoopDefaultMode);

finish:
    if(rlser) CFRelease(rlser);
    if(r_rlser) CFRelease(r_rlser);
    if(notify_port) IOObjectRelease((io_object_t)notify_port);
    if(r_notify_port) IOObjectRelease((io_object_t)r_notify_port);
    if(pmu_service_ref) IOObjectRelease(pmu_service_ref);
    if(root_domain_ref) IOObjectRelease(root_domain_ref);
    return;
}

static bool
systemHasSMU(void)
{
    static io_registry_entry_t              smuRegEntry = MACH_PORT_NULL;
    static bool                             known = false;

    if(known) return (smuRegEntry?true:false);

    smuRegEntry = (io_registry_entry_t)IOServiceGetMatchingService(0,
                        IOServiceNameMatching("AppleSMU"));
    if(MACH_PORT_NULL == smuRegEntry)
    {
        // SMU not supported on this platform, no need to install tz handler
        known = true;
        return false;
    }
    IOObjectRelease(smuRegEntry);
    known = true;
    return true;
}

/* intializeDisplaySleepNotifications
 *
 * Notifications on display sleep. Our only purpose for listening to these
 * is to tell the SMU what our timezone offset is when display sleep kicks
 * in. As such, we only install the notifications on machines with an SMU.
 */
static void
intializeDisplaySleepNotifications(void)
{
    IONotificationPortRef       note_port = MACH_PORT_NULL;
    CFRunLoopSourceRef          dimSrc = NULL;
    io_service_t                display_wrangler = MACH_PORT_NULL;
    io_object_t                 dimming_notification_object = MACH_PORT_NULL;
    IOReturn                    ret;

    if(!systemHasSMU()) return;

    display_wrangler = IOServiceGetMatchingService(NULL, IOServiceNameMatching("IODisplayWrangler"));
    if(!display_wrangler) return;
    
    note_port = IONotificationPortCreate(NULL);
    if(!note_port) return;
    
    ret = IOServiceAddInterestNotification(note_port, display_wrangler, 
                kIOGeneralInterest, displayPowerStateChange,
                NULL, &dimming_notification_object);
    if(ret != kIOReturnSuccess) return;
    
    dimSrc = IONotificationPortGetRunLoopSource(note_port);
    
    if(dimSrc)
    {
        CFRunLoopAddSource(CFRunLoopGetCurrent(), dimSrc, kCFRunLoopDefaultMode);
        CFRelease(dimSrc);
    }
}


/* _ioupsd_exited
 *
 * Gets called (by configd) when /usr/libexec/ioupsd exits
 */
static void _ioupsd_exited(
    pid_t           pid,
    int             status,
    struct rusage   *rusage,
    void            *context)
{
    syslog(LOG_INFO, "PowerManagement: /usr/libexec/ioupsd(%d) has exited with status %d\n", pid, status);
}

void
prime()
{
    char *argv[2] = {"/usr/libexec/ioupsd", NULL};
    pid_t           _ioupsd_pid;
    
    // Initialize battery averaging code
    BatteryTimeRemaining_prime();
    
    // Initialize PMSettings code
    PMSettings_prime();
    
    // Initialize PSLowPower code
    PSLowPower_prime();

    // Initialzie AutoWake code
    AutoWake_prime();
    RepeatingAutoWake_prime();
    
    // Launch iopsd
    _ioupsd_pid = _SCDPluginExecCommand(&_ioupsd_exited, 0, 0, 0,
        "/usr/libexec/ioupsd", argv);
    
    return;
}

void
load(CFBundleRef bundle, Boolean bundleVerbose)
{
    IONotificationPortRef           notify;    
    io_object_t                     anIterator;
    
    // Install notification on Power Source changes
    initializePowerSourceChangeNotification();

    // Install notification when the preferences file changes on disk
    initializeESPrefsDynamicStore();

    // Install notification on ApplePMU&IOPMrootDomain general interest messages
    initializeInterestNotifications();
    
    // Register for display dim/undim notifications
    intializeDisplaySleepNotifications();

    // Register for SystemPower notifications
    _pm_ack_port = IORegisterForSystemPower (0, &notify, SleepWakeCallback, &anIterator);
    if ( _pm_ack_port != NULL ) {
        if(notify) CFRunLoopAddSource(CFRunLoopGetCurrent(),
                            IONotificationPortGetRunLoopSource(notify),
                            kCFRunLoopDefaultMode);
    }

}

// use 'make' to build standalone debuggable executable 'pm'

#ifdef  STANDALONE
int
main(int argc, char **argv)
{
    openlog("pmcfgd", LOG_PID | LOG_NDELAY, LOG_USER);

    load(CFBundleGetMainBundle(), (argc > 1) ? TRUE : FALSE);

    prime();

	CFRunLoopRun();

	/* not reached */
	exit(0);
	return 0;
}
#endif

