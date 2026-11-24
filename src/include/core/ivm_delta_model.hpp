#ifndef IVM_DELTA_MODEL_HPP
#define IVM_DELTA_MODEL_HPP

#include "core/ivm_view_classifier.hpp"

namespace duckdb {

DeltaModelNodeKind NodeKindForOperator(LogicalOperator &op);
// Builds one model node per plan node plus the CREATE-time profile counts. `full_only` marks views for which no
// incremental rule applies to any node (unsupported construct or unrecognized pattern).
void BuildDeltaModelNodes(DeltaViewModel &model, const CreateMVPlanFacts &facts, bool full_only);
void PopulateDeltaViewModelLineage(DeltaViewModel &model, const CreateMVPlanFacts &facts,
                                   const vector<string> &output_names);
// The lineage_json metadata; entry_count receives the number of lineage entries it holds.
string BuildDeltaViewModelLineageJson(const DeltaViewModel &model, idx_t &entry_count);
string BuildDeltaViewModelProfileDetail(const DeltaViewModel &model, idx_t lineage_entry_count);

} // namespace duckdb

#endif // IVM_DELTA_MODEL_HPP
