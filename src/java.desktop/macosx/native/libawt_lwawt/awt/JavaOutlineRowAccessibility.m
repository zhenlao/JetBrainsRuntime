// Copyright 2000-2020 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license that can be found in the LICENSE file.

#import "JavaOutlineRowAccessibility.h"
#import "JavaAccessibilityUtilities.h"
#import "ThreadUtilities.h"

@implementation JavaOutlineRowAccessibility

- (NSString *)getPlatformAxElementClassName {
    return @"PlatformAxOutlineRow";
}

@synthesize accessibleLevel;

@end

@implementation PlatformAxOutlineRow

- (NSInteger)accessibilityDisclosureLevel {
    return [(JavaOutlineRowAccessibility *)[self javaBase] accessibleLevel];
}

- (BOOL)isAccessibilityDisclosed {
    return isExpanded([ThreadUtilities getJNIEnv], [[self javaBase] axContextWithEnv:[ThreadUtilities getJNIEnv]], [[self javaBase] component]);
}

@end
