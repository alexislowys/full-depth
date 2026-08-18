# analytics

Python readers for the C++ exporter's binary output (`trades.bin`, `l1.bin`,
`symbols.csv`, `meta.txt`). Numpy structured dtypes match the packed C++
structs byte-for-byte; record contract documented in `../docs/export-format.md`.

    python -m fulldepth.verify <export_dir>            # integrity checks
    python -m fulldepth.convert <export_dir> [out_dir] # -> trades.parquet, l1.parquet (zstd)

Install: `pip install -r requirements.txt`. Prices are fixed-point, 4 decimals
(`price / 10_000` = dollars); timestamps are ns since midnight ET.
