
#include "ProcessPBAdaptor.h"

namespace firebuild {
int ProcessPBAdaptor::msg(Process &p, const msg::Open &o)
{
  bool c = (o.has_created())?o.created():false;
  int error = (o.has_error_no())?o.error_no():0;
  return p.open_file(o.file(), o.flags(), o.mode(), o.ret(), c, error);
}

int ProcessPBAdaptor::msg(Process &p, const msg::Close &c)
{
  const int error = (c.has_error_no())?c.error_no():0;
  return p.close_file(c.fd(), error);
}

}
