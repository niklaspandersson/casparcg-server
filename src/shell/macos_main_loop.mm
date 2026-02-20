/*
 * macOS-specific main loop implementation using NSApplication
 * This ensures GCD dispatch_async to main queue is properly processed
 */

#import <Cocoa/Cocoa.h>
#import <dispatch/dispatch.h>

static NSApplication* sharedApp = nil;

extern "C" {

// Initialize NSApplication - must be called once before macos_process_events
void macos_init_app() {
    @autoreleasepool {
        if (sharedApp == nil) {
            // Initialize the NSApplication to enable proper GCD main queue processing
            sharedApp = [NSApplication sharedApplication];
            [sharedApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
            // Don't call [sharedApp run] - we'll manually process events
        }
    }
}

// Run the main dispatch queue for up to timeout_seconds
void macos_process_events(double timeout_seconds) {
    @autoreleasepool {
        if (sharedApp == nil) {
            macos_init_app();
        }

        // Process any pending events with timeout
        NSDate* limitDate = [NSDate dateWithTimeIntervalSinceNow:timeout_seconds];

        // Process all pending events
        while (true) {
            NSEvent* event = [sharedApp nextEventMatchingMask:NSEventMaskAny
                                                    untilDate:limitDate
                                                       inMode:NSDefaultRunLoopMode
                                                      dequeue:YES];
            if (event == nil) {
                break;
            }
            [sharedApp sendEvent:event];
            [sharedApp updateWindows];
        }

        // Also run the run loop briefly to ensure GCD blocks are processed
        [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.001]];
    }
}

} // extern "C"
