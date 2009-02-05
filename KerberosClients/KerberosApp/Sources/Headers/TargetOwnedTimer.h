/*
 * TargetOwnedTimer.h
 *
 * $Header: /cvs/kfm/KerberosClients/KerberosApp/Sources/Headers/TargetOwnedTimer.h,v 1.3 2004/10/22 20:43:49 lxs Exp $
 *
 * Copyright 2004 Massachusetts Institute of Technology.
 * All Rights Reserved.
 *
 * Export of this software from the United States of America may
 * require a specific license from the United States Government.
 * It is the responsibility of any person or organization contemplating
 * export to obtain such a license before exporting.
 * 
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 */

@interface TargetOwnedTimer : NSObject
{
    id timerTarget;
    SEL timerSelector;
    id timerUserInfo;
    NSTimer *timer;
}

+ (TargetOwnedTimer *) scheduledTimerWithTimeInterval: (NSTimeInterval) seconds 
                                               target: (id) target 
                                             selector: (SEL) selector 
                                             userInfo: (id) userInfo 
                                              repeats: (BOOL) repeats;

+ (TargetOwnedTimer *) scheduledTimerWithFireDate: (NSDate *) fireDate
                                         interval: (NSTimeInterval) seconds 
                                           target: (id) target 
                                         selector: (SEL) selector 
                                         userInfo: (id) userInfo 
                                          repeats: (BOOL) repeats;

- (id) initWithTimeInterval: (NSTimeInterval) seconds 
                     target: (id) target 
                   selector: (SEL) selector 
                   userInfo: (id) userInfo 
                    repeats: (BOOL) repeats;

- (id) initWithFireDate: (NSDate *) fireDate
               interval: (NSTimeInterval) seconds
                 target: (id) target 
               selector: (SEL) selector 
               userInfo: (id) userInfo 
                repeats: (BOOL) repeats;

- (void) dealloc;

- (NSTimer *) timer;

- (void) invalidate;
- (BOOL) isValid;

- (void) fire;
- (NSDate *) fireDate;
- (void) setFireDate: (NSDate *) date;

- (NSTimeInterval) timeInterval;

- (id) userInfo;

- (void) timer: (NSTimer *) timer;

@end