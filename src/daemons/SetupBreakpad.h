/* Copyright (c) 2026 vesoft inc. All rights reserved.
 *
 * This source code is licensed under Apache 2.0 License.
 */

#ifndef DAEMONS_SETUPBREAKPAD_H_
#define DAEMONS_SETUPBREAKPAD_H_

#include "common/base/Status.h"

nebula::Status setupBreakpad();
nebula::Status setupBreakpadSignalMinidump();

#endif  // DAEMONS_SETUPBREAKPAD_H_
