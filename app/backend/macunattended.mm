#include "macunattended.h"
#import <ServiceManagement/ServiceManagement.h>
static SMAppService* recoveryService() API_AVAILABLE(macos(13.0));
static SMAppService* recoveryService() {
    return [SMAppService daemonServiceWithPlistName:@"io.github.keithxc.DeskPort.Recovery.plist"];
}
int deskPortUnattendedServiceStatus() {
    @autoreleasepool {
        if (@available(macOS 13.0, *)) return static_cast<int>(recoveryService().status);
        return 0;
    }
}
bool deskPortSetUnattendedService(bool enabled, QString& error) {
    @autoreleasepool {
        if (@available(macOS 13.0, *)) {
            SMAppService* service = recoveryService();
            if ((enabled && service.status == SMAppServiceStatusEnabled) ||
                (!enabled && service.status == SMAppServiceStatusNotRegistered)) return true;
            NSError* nativeError = nil;
            const BOOL result = enabled ? [service registerAndReturnError:&nativeError]
                                        : [service unregisterAndReturnError:&nativeError];
            // A pending approval is an actionable state, never an "enabled" success.
            if (enabled && service.status == SMAppServiceStatusRequiresApproval) return true;
            if (!result) error = QString::fromUtf8(nativeError.localizedDescription.UTF8String);
            return result;
        }
        if (!enabled) return true;
        error = QStringLiteral("Background recovery requires macOS 13 or newer. Sharing remains available while DeskPort is running.");
        return false;
    }
}
void deskPortOpenBackgroundItems() {
    if (@available(macOS 13.0, *)) [SMAppService openSystemSettingsLoginItems];
}
