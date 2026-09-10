#pragma once

#include <memory_resource>
#include <string_view>
#include <ruvia/web/db/DbRepository.h>

namespace service::common::database {

template <typename Entity> ruvia::DbPredicate activeId(std::string_view id) {
    return Entity::template column<"id">() == id &&
           Entity::template column<"deleted_at">().isNull();
}

inline ruvia::DbExpression emptyText(ruvia::DbQuery &query, std::string_view column,
                                     std::string_view table = {}) {
    return query.coalesce(
        {query.cast(query.column(column, table), ruvia::DbDataType::kText), query.value("")});
}

inline ruvia::DbExpression nullableUuid(ruvia::DbQuery &query, std::string_view value) {
    return query.cast(query.nullIf(query.value(value), query.value("")), ruvia::DbDataType::kUuid);
}

} // namespace service::common::database
