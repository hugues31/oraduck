#pragma once

#include "oraduck/oci_session.hpp"
#include "oraduck/oracle_names.hpp"

namespace oraduck {

// Resolves the owner (current schema when absent) and describes the table.
// Throws OraduckError when the table does not exist or is not accessible.
TableDescription DescribeTarget(OciSession &session, const TargetName &name);

} // namespace oraduck
