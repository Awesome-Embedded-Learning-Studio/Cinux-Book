/**
 * @file    undocumented_types.h
 * @brief   Test undocumented enums, structs, and classes.
 *
 * @author  Test
 * @date    2024-01-01
 * @version 1.0.0
 * @since   1.0.0
 * @ingroup test
 */

#ifndef UNDOCUMENTED_TYPES_H
#define UNDOCUMENTED_TYPES_H
#define CF_API
// Undocumented enum
enum class UndocumentedEnum {
    Value1,
    Value2,
};

// Undocumented struct
struct UndocumentedStruct {
    int   x;
    float y;
};

// Documented enum
/**
 * @brief  A documented enum.
 * @ingroup test
 */
enum class DocumentedEnum {
    Value1,  ///< First value.
    Value2,  ///< Second value.
};

// Documented struct
/**
 * @brief  A documented struct.
 * @ingroup test
 */
struct DocumentedStruct {
    int   x;  ///< X coordinate.
    float y;  ///< Y coordinate.
};

// Undocumented class
class UndocumentedClass {
public:
    int value;
};

// Documented class
/**
 * @brief  A documented class.
 * @ingroup test
 */
class DocumentedClass {
public:
    int value;  ///< The value.
};

// Undocumented class with export macro
class CF_API UndocumentedClassWithMacro {
public:
    void method();
};

#endif  // UNDOCUMENTED_TYPES_H
