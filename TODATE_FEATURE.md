# /todate Feature Implementation

## Overview
Added a new `/todate:dd-MMM-yy` command-line option to rt11dir.cpp to specify a custom date for files copied to RT-11 disk images. This addresses issues with post-2000 dates in the RT-11 date format.

## Changes Made

### 1. New Date Parsing Function
```cpp
bool parseDateString(const std::string& dateStr, int& day, int& month, int& year)
```
- Parses date strings in format `dd-MMM-yy` (e.g., "15-JAN-97" or "01-DEC-99")
- Validates day (1-31), month (JAN-DEC), and year
- Interprets 2-digit years:
  - 72-99 ? 1972-1999
  - 00-71 ? 2000-2071
- Returns true if parsing successful, false otherwise

### 2. New Date Encoding Function
```cpp
uint16_t encodeRt11Date(int year, int month, int day)
```
- Encodes a specific date into RT-11's packed date format
- Supports years 1972-2099
- Uses the same encoding logic as `encodeRt11DateFromSystem()` but with explicit date values

### 3. Modified Functions
- **copySingleToRt11()**: Added optional `uint16_t optionalDateWord = 0` parameter
  - If provided (non-zero), uses the specified date
  - Otherwise falls back to system date via `encodeRt11DateFromSystem()`
  
- **copyToRt11()**: Added optional `uint16_t optionalDateWord = 0` parameter
  - Passes the date through to all `copySingleToRt11()` calls

### 4. Command-Line Parsing
Updated `main()` to:
- Parse `/todate:dd-MMM-yy` argument
- Validate the date format
- Encode it to RT-11 format
- Display the parsed date for user confirmation
- Pass it to `copyToRt11()`

### 5. Updated Help Text
Added documentation for the `/todate` option explaining:
- Format requirements (dd-MMM-yy)
- Year interpretation rules
- Valid date range (1972-2099)
- Example usage

## Usage Examples

### Copy single file with specific date
```
rt11dir disk.dsk /copyto /from:file.txt /todate:15-JAN-97
```

### Copy multiple files with specific date
```
rt11dir disk.dsk /copyto /from:*.txt /todate:01-DEC-99
```

### Copy with noreplace and custom date
```
rt11dir disk.dsk /copyto /from:*.sav /todate:31-MAR-00 /noreplace
```

## RT-11 Date Format
The RT-11 date format is a 16-bit packed word:
- Bits 14-15: Age (2 bits) - represents 32-year periods from 1972
- Bits 10-13: Month (4 bits, 1-12)
- Bits 5-9: Day (5 bits, 1-31)
- Bits 0-4: Year offset within age period (5 bits, 0-31)

Formula: `year = 1972 + (age * 32) + year_offset`

## Date Interpretation Rules
- **Two-digit years 72-99**: Mapped to 1972-1999
  - Example: "97" ? 1997
  
- **Two-digit years 00-71**: Mapped to 2000-2071
  - Example: "00" ? 2000
  - Example: "15" ? 2015

This matches common Y2K-compatible date handling practices.

## Error Handling
The implementation validates:
- Date string format (must be exactly 9 characters: dd-MMM-yy)
- Day range (1-31)
- Month name (must be valid 3-letter abbreviation)
- Year range (must result in 1972-2099)
- Separator characters (must be hyphens at positions 2 and 6)

Invalid dates result in clear error messages and program termination before any disk modification.

## Testing Recommendations
1. Test with dates in the 1970s-1990s range
2. Test with post-2000 dates (00-71)
3. Test invalid formats to verify error handling
4. Verify dates display correctly in directory listings
5. Test with wildcards copying multiple files
6. Test recursive copying with /todate option

## Benefits
- Allows backdating files to match original creation dates
- Avoids issues with post-2000 dates in older RT-11 versions
- Maintains compatibility with RT-11 date format limitations
- Provides explicit control over file dates in archival scenarios
