// Copyright 2000-2020 JetBrains s.r.o. Use of this source code is governed by the Apache 2.0 license that can be found in the LICENSE file.

#include "jni.h"

#import <AppKit/AppKit.h>

//#define JAVA_AX_DEBUG 1
//#define JAVA_AX_NO_IGNORES 1
//#define JAVA_AX_DEBUG_PARMS 1

// these constants are duplicated in CAccessibility.java
#define JAVA_AX_ALL_CHILDREN (-1)
#define JAVA_AX_SELECTED_CHILDREN (-2)
#define JAVA_AX_VISIBLE_CHILDREN (-3)
// If the value is >=0, it's an index

@class JavaBaseAccessibility;

@protocol JavaBaseProvider

@property (nonatomic, retain) JavaBaseAccessibility *javaBase;

@end

@protocol PlatformAxElementProvider
@required

- (NSString *)getPlatformAxElementClassName;

@property (nonatomic, retain) NSObject <JavaBaseProvider> *platformAxElement;

@end

@interface JavaBaseAccessibility : NSObject <JavaBaseProvider, PlatformAxElementProvider> {
    NSView *fView;
    NSObject *fParent;

    NSString *fNSRole;
    NSString *fJavaRole;

    jint fIndex;
    jobject fAccessible;
    jobject fComponent;
}

- (id)initWithParent:(NSObject*)parent withEnv:(JNIEnv *)env withAccessible:(jobject)accessible withIndex:(jint)index withView:(NSView *)view withJavaRole:(NSString *)javaRole;
- (void)unregisterFromCocoaAXSystem;
- (void)postValueChanged;
- (void)postSelectedTextChanged;
- (void)postSelectionChanged;
- (BOOL)isEqual:(id)anObject;
- (BOOL)isAccessibleWithEnv:(JNIEnv *)env forAccessible:(jobject)accessible;

+ (void)postFocusChanged:(id)message;

+ (NSArray*)childrenOfParent:(JavaBaseAccessibility*)parent withEnv:(JNIEnv *)env withChildrenCode:(NSInteger)whichChildren allowIgnored:(BOOL)allowIgnored;
+ (NSArray*)childrenOfParent:(JavaBaseAccessibility*)parent withEnv:(JNIEnv *)env withChildrenCode:(NSInteger)whichChildren allowIgnored:(BOOL)allowIgnored recursive:(BOOL)recursive;
+ (JavaBaseAccessibility *) createWithParent:(JavaBaseAccessibility *)parent accessible:(jobject)jaccessible role:(NSString *)javaRole index:(jint)index withEnv:(JNIEnv *)env withView:(NSView *)view isWrapped:(BOOL)wrapped;
+ (JavaBaseAccessibility *) createWithParent:(JavaBaseAccessibility *)parent accessible:(jobject)jaccessible role:(NSString *)javaRole index:(jint)index withEnv:(JNIEnv *)env withView:(NSView *)view;
+ (JavaBaseAccessibility *) createWithAccessible:(jobject)jaccessible role:(NSString *)role index:(jint)index withEnv:(JNIEnv *)env withView:(NSView *)view;
+ (JavaBaseAccessibility *) createWithAccessible:(jobject)jaccessible withEnv:(JNIEnv *)env withView:(NSView *)view;

@property(readonly) jobject accessible;
@property(readonly) jobject component;
@property(readonly) jint index;

- (jobject)axContextWithEnv:(JNIEnv *)env;
- (NSView*)view;
- (NSWindow*)window;
- (id)parent;
-(void)setParent:(id)javaBaseAccessibilityParent;
- (NSString *)javaRole;
- (NSString *)nsRole;
- (BOOL)isMenu;
- (BOOL)isSelected:(JNIEnv *)env;
- (BOOL)isSelectable:(JNIEnv *)env;
- (BOOL)isVisible:(JNIEnv *)env;
- (NSSize)getSize;
- (NSRect)getBounds;
- (id)getFocusedElement;

@end
