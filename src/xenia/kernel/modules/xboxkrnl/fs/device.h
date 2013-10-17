/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_MODULES_XBOXKRNL_FS_DEVICE_H_
#define XENIA_KERNEL_MODULES_XBOXKRNL_FS_DEVICE_H_

#include <xenia/common.h>
#include <xenia/core.h>

#include <xenia/kernel/modules/xboxkrnl/fs/entry.h>


namespace xe {
namespace kernel {
namespace xboxkrnl {
namespace fs {


class Device {
public:
  Device(const char* path);
  virtual ~Device();

  const char* path();

  virtual Entry* ResolvePath(const char* path) = 0;

protected:
  char*       path_;
};


}  // namespace fs
}  // namespace xboxkrnl
}  // namespace kernel
}  // namespace xe


#endif  // XENIA_KERNEL_MODULES_XBOXKRNL_FS_DEVICE_H_