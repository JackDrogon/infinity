//
// Created by jinhai on 23-3-13.
//

#pragma once

#include <utility>

#include "executor/physical_operator.h"
#include "parser/statement/explain_statement.h"

namespace infinity {

class PhysicalExplain final : public PhysicalOperator {
public:
    explicit PhysicalExplain(u64 id,
                             ExplainType type,
                             SharedPtr<Vector<SharedPtr<String>>> text_array,
                             SharedPtr<PhysicalOperator> left)
            : PhysicalOperator(PhysicalOperatorType::kExplain, std::move(left), nullptr, id),
            explain_type_(type),
            texts_(std::move(text_array))
            {}

    ~PhysicalExplain() override = default;

    void
    Init() override;

    void
    Execute(SharedPtr<QueryContext>& query_context) override;

    inline SharedPtr<Vector<String>>
    GetOutputNames() const final {
        return MakeShared<Vector<String>>();
    }

    inline SharedPtr<Vector<SharedPtr<DataType>>>
    GetOutputTypes() const final {
        return MakeShared<Vector<SharedPtr<DataType>>>();
    }

private:
    ExplainType explain_type_{ExplainType::kPhysical};
    SharedPtr<Vector<SharedPtr<String>>> texts_{nullptr};
};

}
