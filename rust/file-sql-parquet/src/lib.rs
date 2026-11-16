//! Streaming Parquet decoding for seekdb's native file SQL scan operator.
//! SQL planning and execution stay in seekdb; this crate only decodes pages.

use parquet::basic::{ConvertedType, Type as PhysicalType};
use parquet::file::reader::{FileReader, SerializedFileReader};
use parquet::record::reader::RowIter;
use parquet::record::Field;
use std::ffi::c_void;
use std::fs::File;
use std::os::fd::FromRawFd;
use std::ptr;

struct Column {
    name: Vec<u8>,
    kind: i32,
    nullable: bool,
}

struct Handle {
    columns: Vec<Column>,
    rows: RowIter<'static>,
    current: Option<Vec<Option<Vec<u8>>>>,
}

fn column_kind(physical: PhysicalType, converted: ConvertedType) -> Option<i32> {
    match converted {
        ConvertedType::DATE => return Some(4),
        ConvertedType::TIMESTAMP_MILLIS | ConvertedType::TIMESTAMP_MICROS => return Some(5),
        ConvertedType::UINT_64 => return Some(6),
        ConvertedType::NONE | ConvertedType::UTF8 | ConvertedType::ENUM
        | ConvertedType::UINT_8 | ConvertedType::UINT_16 | ConvertedType::UINT_32
        | ConvertedType::INT_8 | ConvertedType::INT_16
        | ConvertedType::INT_32 | ConvertedType::INT_64 => {}
        _ => return None,
    }
    match physical {
        PhysicalType::BOOLEAN => Some(1),
        PhysicalType::INT32 | PhysicalType::INT64 => Some(2),
        PhysicalType::FLOAT | PhysicalType::DOUBLE => Some(3),
        PhysicalType::BYTE_ARRAY | PhysicalType::FIXED_LEN_BYTE_ARRAY => Some(6),
        _ => None,
    }
}

// Civil date conversion from days since 1970-01-01, including negative days.
fn civil_date(days: i64) -> (i64, i64, i64) {
    let z = days + 719_468;
    let era = if z >= 0 { z } else { z - 146_096 } / 146_097;
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365;
    let mut year = yoe + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let day = doy - (153 * mp + 2) / 5 + 1;
    let month = mp + if mp < 10 { 3 } else { -9 };
    if month <= 2 {
        year += 1;
    }
    (year, month, day)
}

fn date_text(days: i64) -> String {
    let (year, month, day) = civil_date(days);
    format!("{year:04}-{month:02}-{day:02}")
}

fn timestamp_text(micros: i64) -> String {
    const DAY_MICROS: i64 = 86_400_000_000;
    let day = micros.div_euclid(DAY_MICROS);
    let rem = micros.rem_euclid(DAY_MICROS);
    let hours = rem / 3_600_000_000;
    let minutes = rem / 60_000_000 % 60;
    let seconds = rem / 1_000_000 % 60;
    let fraction = rem % 1_000_000;
    let date = date_text(day);
    if fraction == 0 {
        format!("{date} {hours:02}:{minutes:02}:{seconds:02}")
    } else {
        format!("{date} {hours:02}:{minutes:02}:{seconds:02}.{fraction:06}")
    }
}

fn field_text(field: &Field) -> Option<Option<Vec<u8>>> {
    let text = match field {
        Field::Null => return Some(None),
        Field::Bool(v) => v.to_string(),
        Field::Byte(v) => v.to_string(),
        Field::Short(v) => v.to_string(),
        Field::Int(v) => v.to_string(),
        Field::Long(v) => v.to_string(),
        Field::UByte(v) => v.to_string(),
        Field::UShort(v) => v.to_string(),
        Field::UInt(v) => v.to_string(),
        Field::ULong(v) => v.to_string(),
        Field::Float16(v) => v.to_f32().to_string(),
        Field::Float(v) => v.to_string(),
        Field::Double(v) => v.to_string(),
        Field::Str(v) => v.clone(),
        Field::Bytes(v) => String::from_utf8(v.data().to_vec()).ok()?,
        Field::Date(v) => date_text(i64::from(*v)),
        Field::TimestampMillis(v) => timestamp_text(v.checked_mul(1_000)?),
        Field::TimestampMicros(v) => timestamp_text(*v),
        _ => return None,
    };
    Some(Some(text.into_bytes()))
}

#[no_mangle]
pub unsafe extern "C" fn fs_parquet_open_fd(fd: i32) -> *mut c_void {
    if fd < 0 {
        return ptr::null_mut();
    }
    // The C++ caller transfers a safely opened regular-file descriptor.
    let file = unsafe { File::from_raw_fd(fd) };
    let reader = match SerializedFileReader::new(file) {
        Ok(reader) => reader,
        Err(_) => return ptr::null_mut(),
    };
    let schema = reader.metadata().file_metadata().schema_descr();
    let mut columns = Vec::with_capacity(schema.num_columns());
    if schema.num_columns() == 0 || schema.num_columns() > 4_096 {
        return ptr::null_mut();
    }
    for descriptor in schema.columns() {
        if descriptor.path().parts().len() != 1 || descriptor.max_rep_level() != 0 {
            return ptr::null_mut();
        }
        let kind = match column_kind(descriptor.physical_type(), descriptor.converted_type()) {
            Some(kind) => kind,
            None => return ptr::null_mut(),
        };
        columns.push(Column {
            name: descriptor.name().as_bytes().to_vec(),
            kind,
            nullable: descriptor.max_def_level() > 0,
        });
    }
    let rows = RowIter::from_file_into(Box::new(reader));
    Box::into_raw(Box::new(Handle { columns, rows, current: None })).cast()
}

#[no_mangle]
pub unsafe extern "C" fn fs_parquet_close(handle: *mut c_void) {
    if !handle.is_null() {
        drop(unsafe { Box::from_raw(handle.cast::<Handle>()) });
    }
}

#[no_mangle]
pub unsafe extern "C" fn fs_parquet_column_count(handle: *const c_void) -> i64 {
    unsafe { handle.cast::<Handle>().as_ref() }
        .map_or(-1, |handle| handle.columns.len() as i64)
}

fn column(handle: &Handle, index: i64) -> Option<&Column> {
    if index < 0 {
        return None;
    }
    handle.columns.get(index as usize)
}

#[no_mangle]
pub unsafe extern "C" fn fs_parquet_column_name_len(handle: *const c_void, index: i64) -> i64 {
    unsafe { handle.cast::<Handle>().as_ref() }
        .and_then(|handle| column(handle, index))
        .map_or(-1, |column| column.name.len() as i64)
}

#[no_mangle]
pub unsafe extern "C" fn fs_parquet_column_name_copy(
    handle: *const c_void, index: i64, out: *mut u8, capacity: usize,
) -> i32 {
    match (unsafe { handle.cast::<Handle>().as_ref() }).and_then(|handle| column(handle, index)) {
        Some(column) if !out.is_null() && capacity >= column.name.len() => {
            unsafe { ptr::copy_nonoverlapping(column.name.as_ptr(), out, column.name.len()) };
            0
        }
        _ => -1,
    }
}

#[no_mangle]
pub unsafe extern "C" fn fs_parquet_column_type(handle: *const c_void, index: i64) -> i32 {
    unsafe { handle.cast::<Handle>().as_ref() }
        .and_then(|handle| column(handle, index))
        .map_or(-1, |column| column.kind)
}

#[no_mangle]
pub unsafe extern "C" fn fs_parquet_column_nullable(handle: *const c_void, index: i64) -> i32 {
    unsafe { handle.cast::<Handle>().as_ref() }
        .and_then(|handle| column(handle, index))
        .map_or(-1, |column| i32::from(column.nullable))
}

#[no_mangle]
pub unsafe extern "C" fn fs_parquet_next(handle: *mut c_void) -> i32 {
    let Some(handle) = (unsafe { handle.cast::<Handle>().as_mut() }) else {
        return -1;
    };
    handle.current = None;
    let row = match handle.rows.next() {
        Some(Ok(row)) => row,
        Some(Err(_)) => return -1,
        None => return 1,
    };
    let mut values = Vec::with_capacity(handle.columns.len());
    for (_, field) in row.get_column_iter() {
        let Some(value) = field_text(field) else {
            return -1;
        };
        values.push(value);
    }
    if values.len() != handle.columns.len() {
        return -1;
    }
    handle.current = Some(values);
    0
}

fn value(handle: &Handle, index: i64) -> Option<&Option<Vec<u8>>> {
    if index < 0 {
        return None;
    }
    handle.current.as_ref()?.get(index as usize)
}

#[no_mangle]
pub unsafe extern "C" fn fs_parquet_value_len(handle: *const c_void, index: i64) -> i64 {
    match (unsafe { handle.cast::<Handle>().as_ref() }).and_then(|handle| value(handle, index)) {
        Some(Some(value)) => value.len() as i64,
        Some(None) => -1,
        None => -2,
    }
}

#[no_mangle]
pub unsafe extern "C" fn fs_parquet_value_copy(
    handle: *const c_void, index: i64, out: *mut u8, capacity: usize,
) -> i32 {
    match (unsafe { handle.cast::<Handle>().as_ref() }).and_then(|handle| value(handle, index)) {
        Some(Some(value)) if !out.is_null() && capacity >= value.len() => {
            unsafe { ptr::copy_nonoverlapping(value.as_ptr(), out, value.len()) };
            0
        }
        _ => -1,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use base64::Engine;
    use parquet::basic::Compression;
    use parquet::data_type::Int32Type;
    use parquet::file::properties::WriterProperties;
    use parquet::file::writer::SerializedFileWriter;
    use parquet::schema::parser::parse_message_type;
    use std::os::fd::IntoRawFd;
    use std::sync::Arc;

    #[test]
    fn reads_parquet_fixture_rows() {
        let encoded = include_str!("../../../tools/deploy/mysql_test/test_suite/file_sql/data/simple.parquet.b64");
        let bytes = base64::engine::general_purpose::STANDARD
            .decode(encoded.trim())
            .unwrap();
        let path = std::env::temp_dir().join(format!("seekdb-parquet-{}.parquet", std::process::id()));
        std::fs::write(&path, bytes).unwrap();
        let fd = File::open(&path).unwrap().into_raw_fd();
        let handle = unsafe { fs_parquet_open_fd(fd) };
        assert!(!handle.is_null());
        assert_eq!(unsafe { fs_parquet_column_count(handle) }, 1);
        assert_eq!(unsafe { fs_parquet_column_type(handle, 0) }, 2);
        assert_eq!(unsafe { fs_parquet_column_nullable(handle, 0) }, 1);
        for i in 0..7 {
            assert_eq!(unsafe { fs_parquet_next(handle) }, 0);
            let len = unsafe { fs_parquet_value_len(handle, 0) };
            let mut value = vec![0; len as usize];
            assert_eq!(unsafe { fs_parquet_value_copy(handle, 0, value.as_mut_ptr(), value.len()) }, 0);
            assert_eq!(value, i.to_string().as_bytes());
        }
        assert_eq!(unsafe { fs_parquet_next(handle) }, 1);
        unsafe { fs_parquet_close(handle) };
        std::fs::remove_file(path).unwrap();
    }

    #[test]
    fn reads_snappy_nullable_across_row_groups() {
        let path = std::env::temp_dir().join(format!(
            "seekdb-parquet-multi-{}.parquet", std::process::id()
        ));
        let schema = Arc::new(parse_message_type("message schema { OPTIONAL INT32 value; }").unwrap());
        let properties = Arc::new(
            WriterProperties::builder()
                .set_compression(Compression::SNAPPY)
                .build(),
        );
        let file = File::create(&path).unwrap();
        let mut writer = SerializedFileWriter::new(file, schema, properties).unwrap();
        for (values, levels) in [(&[1][..], &[1, 0][..]), (&[2][..], &[1][..])] {
            let mut group = writer.next_row_group().unwrap();
            let mut column = group.next_column().unwrap().unwrap();
            column.typed::<Int32Type>().write_batch(values, Some(levels), None).unwrap();
            column.close().unwrap();
            group.close().unwrap();
        }
        writer.close().unwrap();

        let handle = unsafe { fs_parquet_open_fd(File::open(&path).unwrap().into_raw_fd()) };
        assert!(!handle.is_null());
        assert_eq!(unsafe { fs_parquet_column_count(handle) }, 1);
        assert_eq!(unsafe { fs_parquet_column_nullable(handle, 0) }, 1);
        assert_eq!(unsafe { fs_parquet_next(handle) }, 0);
        assert_eq!(unsafe { fs_parquet_value_len(handle, 0) }, 1);
        assert_eq!(unsafe { fs_parquet_next(handle) }, 0);
        assert_eq!(unsafe { fs_parquet_value_len(handle, 0) }, -1);
        assert_eq!(unsafe { fs_parquet_next(handle) }, 0);
        assert_eq!(unsafe { fs_parquet_value_len(handle, 0) }, 1);
        assert_eq!(unsafe { fs_parquet_next(handle) }, 1);
        unsafe { fs_parquet_close(handle) };
        std::fs::remove_file(path).unwrap();
    }
}
