// Fault injection for the test build (SFP_TEST_HOOKS). Never in a release.
#ifndef SFP_TEST_HOOKS_H
#define SFP_TEST_HOOKS_H

#include <stdint.h>

// The next n I2C transfer attempts fail as if the module NACKed.
void sfp_test_inject_nack(uint32_t n);
// Hold SDA low for ms milliseconds through a spare GPIO, the way a module
// pulled out mid-byte does.
void sfp_test_inject_sda_low(uint32_t ms);

#endif
