#import <UIKit/UIKit.h>

static UIImpactFeedbackGenerator *generator;

void port_ios_haptic(void) {
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!generator) generator = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
        [generator impactOccurred];
        [generator prepare];
    });
}
