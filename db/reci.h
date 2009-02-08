// reci.h

#pragma once

#include "storage.h"

namespace mongo { 

/* Subclass this and implement your real storage interface.
   Currently we just use a single storage interface at compile time.  Later if desired to be runtime 
   configurable we will make these pure virtual functions / nonstatic.
*/
class RecStoreInterface {
public:
    static char* get(DiskLoc d, unsigned len) { assert(false); return 0; }

    static void modified(DiskLoc d) { assert(false); }

    /* insert specified data as a record */
    static DiskLoc insert(const char *ns, const void *obuf, int len, bool god) { assert(false); return DiskLoc(); }

};

}