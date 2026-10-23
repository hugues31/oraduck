#pragma once

#include "oraduck/describe_source.hpp"
#include "oraduck/oci_session.hpp"
#include "oraduck/read_sql.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace oraduck {

// Work units of one query execution. Computed at each execution and never cached: ROWID ranges go stale after a
// TRUNCATE or new partitions.
struct WorkPlan {
	SplitStrategy strategy = SplitStrategy::kNone; // never kAuto
	std::vector<Slice> slices;                     // none: nothing to read (table without segment)
	std::string scn;                               // empty: no AS OF SCN
	std::string notes;                             // extent source and fallbacks, "; "-separated
};

// Current SCN as text from DBMS_FLASHBACK.GET_SYSTEM_CHANGE_NUMBER, else V$DATABASE.CURRENT_SCN; "" when the user may
// read neither. TIMESTAMP_TO_SCN(SYSTIMESTAMP) is never used: it lags a few seconds behind the commits.
std::string ReadCurrentScn(OciSession &session);

// Snapshot SCN and slices for reading source with up to `sessions` sessions. Without a current SCN or when Oracle
// refuses AS OF SCN, snapshot 'auto' reads per session (noted) and 'scn' throws. A forced strategy that cannot run
// throws OraduckError (OracleError when Oracle refused). Extents from DBMS_PARALLEL_EXECUTE create and drop a task,
// which commits the session's transaction.
WorkPlan PlanWork(OciSession &session, const OracleTableDescription &source, SplitStrategy split, SnapshotMode snapshot,
                  uint32_t sessions);

} // namespace oraduck
