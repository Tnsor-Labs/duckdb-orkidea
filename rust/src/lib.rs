//! The Orkidea Oracle ADBC driver, linked statically into the DuckDB
//! extension. The extension calls the driver through its ADBC entry point.

adbc_ffi::export_driver!(OrkideaDriverInit, oracle_adbc::OracleDriver);
