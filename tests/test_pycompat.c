#include "testutil.h"
#include "utils/pycompat.h"

int main(void) {
    CHECK_EQ_INT(py_round_half_even(0.5), 0);
    CHECK_EQ_INT(py_round_half_even(1.5), 2);
    CHECK_EQ_INT(py_round_half_even(2.5), 2);
    CHECK_EQ_INT(py_round_half_even(3.5), 4);
    CHECK_EQ_INT(py_round_half_even(-0.5), 0);
    CHECK_EQ_INT(py_round_half_even(-1.5), -2);
    CHECK_EQ_INT(py_round_half_even(-2.5), -2);
    CHECK_EQ_INT(py_round_half_even(2.675), 3);
    CHECK_EQ_INT(py_round_half_even(-2.6), -3);
    CHECK_EQ_INT(py_round_half_even(0.0), 0);

    CHECK_EQ_INT(py_floor_div(7, 2), 3);
    CHECK_EQ_INT(py_floor_div(-7, 2), -4);
    CHECK_EQ_INT(py_floor_div(7, -2), -4);
    CHECK_EQ_INT(py_floor_div(-7, -2), 3);
    CHECK_EQ_INT(py_mod(-7, 3), 2);
    CHECK_EQ_INT(py_mod(7, -3), -2);
    return test_summary("test_pycompat");
}
