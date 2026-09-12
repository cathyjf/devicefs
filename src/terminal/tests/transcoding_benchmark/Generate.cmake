# SPDX-FileCopyrightText: Copyright 2026 Cathy J. Fitzpatrick <cathy@cathyjf.com>
# SPDX-License-Identifier: GPL-3.0-or-later

# The experiment generates variants of the production owner so conversion,
# buffer initialization, and destruction stay comparable across policies.
# The generated module is linked only into the benchmark executable.
file(READ "${TERMINAL_SOURCE}/transcoding.ixx" source)
foreach(required
    "export module devicefs.terminal.transcoding;"
    "template <detail::UtfCharacter Character>\nclass TranscodedText"
    "const auto input = std::basic_string_view{units};"
    "const auto capacity = input.empty() ? 0 : detail::TranscodedCapacity<Character>(input);"
)
    string(FIND "${source}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "The transcoding benchmark transformation needs updating: ${required}")
    endif()
endforeach()
string(REPLACE "export module devicefs.terminal.transcoding;"
    "export module devicefs.terminal.transcoding_benchmark;" source "${source}")
string(REPLACE "namespace devicefs::terminal::detail {"
    "export namespace devicefs::terminal {
enum class Sizing { Exact, Worst, Hybrid, RetryExact, RetryWorst };

// UTF-16 can expand to three UTF-8 bytes per code unit. UTF-32 can expand
// to four UTF-8 bytes or two UTF-16 code units. Fixtures use these directions.
template <typename Output, typename Input>
[[nodiscard]] constexpr auto WorstCaseCapacity(const std::basic_string_view<Input> input) {
    constexpr auto factor = sizeof(Output) == 1 ? sizeof(Input) == 2 ? 3uz : 4uz : 2uz;
    return input.size() * factor;
}
}
namespace devicefs::terminal::detail {" source "${source}")
string(REPLACE "template <detail::UtfCharacter Character>\nclass TranscodedText"
    "template <detail::UtfCharacter Character, Sizing Policy = Sizing::Exact>\nclass TranscodedText" source "${source}")
string(REPLACE "const auto input = std::basic_string_view{units};"
    "const auto input = std::basic_string_view{units};
        if constexpr (Policy == Sizing::RetryExact || Policy == Sizing::RetryWorst) {
            const auto attempted = simdutf::benchmark_try_utf16_to_utf8(
                input.data(), input.size(), inline_.data(), inline_.size() - 1);
            if (attempted.error == simdutf::SUCCESS) {
                size_ = attempted.output_count;
                inline_[size_] = Character{};
                return;
            }
            if (attempted.error != simdutf::OUTPUT_BUFFER_TOO_SMALL) {
                throw std::invalid_argument(\"invalid benchmark input\");
            }
        }" source "${source}")
string(REPLACE "const auto capacity = input.empty() ? 0 : detail::TranscodedCapacity<Character>(input);"
    "const auto capacity = input.empty() ? 0 : [&] {
            const auto bound = WorstCaseCapacity<Character>(input);
            if constexpr (Policy == Sizing::Worst || Policy == Sizing::RetryWorst) { return bound; }
            else {
                if constexpr (Policy == Sizing::Hybrid) {
                    if (bound < inline_.size() || input.size() >= inline_.size()) { return bound; }
                }
                return detail::TranscodedCapacity<Character>(input);
            }
        }();" source "${source}")
file(READ "${CMAKE_CURRENT_LIST_DIR}/exports.cpp.in" exports)
file(WRITE "${OUTPUT_DIRECTORY}/sizing.ixx" "${source}\n${exports}")
execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DSOURCE_DIRECTORY=${TERMINAL_SOURCE}/dependencies/simdutf"
    "-DOUTPUT=${OUTPUT_DIRECTORY}/generated/simdutf/combined.h"
    -P "${TERMINAL_SOURCE}/cmake/GenerateSimdutf.cmake"
    COMMAND_ERROR_IS_FATAL ANY)

# The upstream bounded converter discards its completion status. This experimental
# copy retains it, allowing the benchmark to distinguish a full inline result
# from a prefix that needs a larger buffer. Its conversion algorithm is unchanged.
file(READ "${TERMINAL_SOURCE}/dependencies/simdutf/simdutf.cpp" upstream)
string(FIND "${upstream}" "simdutf_warn_unused size_t\nconvert_utf16_to_utf8_safe(" begin)
if(begin EQUAL -1)
    message(FATAL_ERROR "The simdutf bounded converter changed; review the retry benchmark")
endif()
string(SUBSTRING "${upstream}" ${begin} -1 bounded)
string(FIND "${bounded}" "\n#endif" end)
string(SUBSTRING "${bounded}" 0 ${end} bounded)
string(REPLACE "simdutf_warn_unused size_t\nconvert_utf16_to_utf8_safe(" "simdutf_warn_unused full_result\nbenchmark_try_utf16_to_utf8(" bounded "${bounded}")
string(REPLACE "const auto start{utf8_output};" "const auto start{utf8_output};\n  const auto initial_len = len;" bounded "${bounded}")
string(REPLACE "return 0; // indicating failure" "return {error_code::OTHER, initial_len - len, size_t(utf8_output - start)};" bounded "${bounded}")
string(FIND "${bounded}" "  if (r.error != error_code::SUCCESS" tail)
if(tail EQUAL -1)
    message(FATAL_ERROR "The simdutf bounded converter's result handling changed; review the retry benchmark")
endif()
string(SUBSTRING "${bounded}" 0 ${tail} bounded)
string(APPEND bounded "  return {r.error, initial_len - len + r.input_count,\n      r.output_count + size_t(utf8_output - start)};\n}\n")
file(READ "${OUTPUT_DIRECTORY}/generated/simdutf/combined.h" combined)
string(REPLACE "\n#undef char16_t" "\nnamespace simdutf {\n${bounded}\n}\n#undef char16_t" combined "${combined}")
file(WRITE "${OUTPUT_DIRECTORY}/generated/simdutf/combined.h" "${combined}")
