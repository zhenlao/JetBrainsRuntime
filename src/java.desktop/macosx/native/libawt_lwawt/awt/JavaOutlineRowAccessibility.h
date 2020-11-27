// Copyright 2000-2020 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license that can be found in the LICENSE file.
#import "JavaListRowAccessibility.h"

@interface JavaOutlineRowAccessibility : JavaListRowAccessibility

@property(readwrite) int accessibleLevel;

@end

@interface PlatformAxOutlineRow : PlatformAxListRow
@end

