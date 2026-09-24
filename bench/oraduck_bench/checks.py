"""Checksums compared between DuckDB and Oracle after every run."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Checksums:
    rows: int
    sum_id: int
    len_label: int
    nulls_amount: int


DUCK_CHECKSUM_SQL = (
    "SELECT count(*), coalesce(sum(id), 0), coalesce(sum(length(label)), 0), count(*) - count(amount) "
    "FROM {table};"
)
ORACLE_CHECKSUM_SQL = (
    "SELECT COUNT(*), NVL(SUM(ID), 0), NVL(SUM(LENGTH(LABEL)), 0), COUNT(*) - COUNT(AMOUNT) FROM {table}"
)
