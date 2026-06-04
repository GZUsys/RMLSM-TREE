#include "util/env_pm.h"

#include <sys/mount.h>
#include <sys/statfs.h>

namespace leveldb {

bool is_dax_filesystem(const std::string &filepath) { // 判断是否为dax文件系统
    if (filepath.find("pmem") != std::string::npos)
            return true; // pmem文件系统路径
    // 非dax文件系统
    return false;
}

bool ends_with_ldb(const std::string &str) {
    if (str.length() < 4)
        return false;
    return str.substr(str.length() - 4) == ".ldb";
}

Status PMError(const std::string& context, int error_number) {
  if (error_number == ENOENT) {
    return Status::NotFound(context, std::strerror(error_number));
  } else {
    return Status::IOError(context, std::strerror(error_number));
  }
}

static Slice Basename(const std::string& filename) {
    std::string::size_type separator_pos = filename.rfind('/');
    if (separator_pos == std::string::npos) {
      return Slice(filename);
    }
    // The filename component should not contain a path separator. If it does,
    // the splitting was done incorrectly.
    assert(filename.find('/', separator_pos + 1) == std::string::npos);

    return Slice(filename.data() + separator_pos + 1,
                 filename.length() - separator_pos - 1);
  }

// True if the given file is a manifest file.
static bool IsManifest(const std::string& filename) {
    return Basename(filename).starts_with("MANIFEST");
}

static std::string Dirname(const std::string& filename) {
    std::string::size_type separator_pos = filename.rfind('/');
    if (separator_pos == std::string::npos) {
        return std::string(".");
    }
    // The filename component should not contain a path separator. If it does,
    // the splitting was done incorrectly.
    assert(filename.find('/', separator_pos + 1) == std::string::npos);

    return filename.substr(0, separator_pos);
}

};
