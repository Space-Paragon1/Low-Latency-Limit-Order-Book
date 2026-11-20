#include "lob/Types.h"

#include <ostream>

namespace lob {

std::ostream& operator<<(std::ostream& os, Side s)          { return os << toString(s); }
std::ostream& operator<<(std::ostream& os, OrderType t)     { return os << toString(t); }
std::ostream& operator<<(std::ostream& os, TimeInForce t)   { return os << toString(t); }
std::ostream& operator<<(std::ostream& os, OrderStatus s)   { return os << toString(s); }
std::ostream& operator<<(std::ostream& os, RejectReason r)  { return os << toString(r); }
std::ostream& operator<<(std::ostream& os, const Symbol& s) { return os << s.view(); }

}  // namespace lob
