#import <UIKit/UIKit.h>

static UIImpactFeedbackGenerator *generator;

void port_ios_haptic(void) {
    if (!generator) generator = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    [generator impactOccurred];
    [generator prepare];
}
