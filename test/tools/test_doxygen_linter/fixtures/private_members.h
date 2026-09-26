/**
 * @file    private_members.h
 * @brief   Test private member handling.
 *
 * @author  Test
 * @date    2024-01-01
 * @version 1.0.0
 * @since   1.0.0
 * @ingroup test
 */

#ifndef PRIVATE_MEMBERS_H
#define PRIVATE_MEMBERS_H

/**
 * @brief  A class with mixed access levels.
 * @ingroup test
 */
class MixedAccessClass {
public:
    /**
     * @brief  Public function.
     *
     * @param[in]  x  Input.
     * @return        Result.
     * @throws        None
     * @note          None
     * @warning       None
     * @since         1.0.0
     * @ingroup       test
     */
    int publicFunc(int x);

    // Undocumented public struct - should fail
    struct PublicStruct {
        int value;
    };

private:
    // Private function - should be skipped
    int privateFunc(int x);

    // Private struct - should be skipped
    struct PrivateStruct {
        int value;
    };

private:
    // Union in private section - should be skipped
    union PrivateUnion {
        int   a;
        float b;
    };

protected:
    /**
     * @brief  Protected function.
     *
     * @param[in]  x  Input.
     * @return        Result.
     * @throws        None
     * @note          None
     * @warning       None
     * @since         1.0.0
     * @ingroup       test
     */
    int protectedFunc(int x);

    // Undocumented protected enum - should fail
    enum class ProtectedEnum {
        Value1,
        Value2,
    };
};

#endif  // PRIVATE_MEMBERS_H
