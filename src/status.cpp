#include "graft/graft.h"

extern "C" const char *graft_status_string(graft_status status) {
    switch (status) {
    case GRAFT_OK:             return "ok";
    case GRAFT_E_INVALID_ARG:  return "invalid argument";
    case GRAFT_E_NOMEM:        return "out of memory";
    case GRAFT_E_IO:           return "I/O error";
    case GRAFT_E_CORRUPT:      return "corrupt input";
    case GRAFT_E_LIMIT:        return "input exceeds safety limit";
    case GRAFT_E_WRONG_SOURCE: return "source does not match patch";
    case GRAFT_E_UNSUPPORTED:  return "unsupported format or feature";
    case GRAFT_STATUS_MAX_ENUM_: break;
    }
    return "unknown status";
}
