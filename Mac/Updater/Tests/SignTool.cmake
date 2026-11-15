# bg3mse-sign: a key made outside any repository, a release zip, the
# channel manifest made from it and checked, the next one, and refusals.
#
#   cmake -DSIGN=<bg3mse-sign> -DZIP_SCRIPT=<Mac/Release/Zip.cmake> -DSOURCE_DIR=<source> -P SignTool.cmake

foreach(name SIGN ZIP_SCRIPT SOURCE_DIR)
    if(NOT DEFINED ${name})
        message(FATAL_ERROR "SignTool.cmake: ${name} is not set")
    endif()
endforeach()

function(run expect)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(expect STREQUAL "ok" AND NOT rc EQUAL 0)
        message(FATAL_ERROR "${ARGN} failed (${rc}):\n${out}${err}")
    elseif(expect STREQUAL "fail" AND rc EQUAL 0)
        message(FATAL_ERROR "${ARGN} did not fail:\n${out}${err}")
    endif()
    set(out "${out}" PARENT_SCOPE)
    set(err "${err}" PARENT_SCOPE)
endfunction()

# Outside any git work tree: the system's temporary directory.
if(DEFINED ENV{TMPDIR})
    set(base "$ENV{TMPDIR}")
else()
    set(base "/tmp")
endif()
string(RANDOM LENGTH 12 suffix)
set(dir "${base}/bg3mse-sign-test-${suffix}")
file(MAKE_DIRECTORY "${dir}")

# A key, refused inside the source tree, as everywhere inside a git work tree.
run(fail "${SIGN}" keygen "${SOURCE_DIR}/Mac/Updater/update.key")
if(EXISTS "${SOURCE_DIR}/Mac/Updater/update.key")
    message(FATAL_ERROR "bg3mse-sign wrote a key inside the source tree")
endif()
if(NOT err MATCHES "git")
    message(FATAL_ERROR "the refusal does not say why: ${err}")
endif()
run(ok "${SIGN}" keygen "${dir}/update.key")
string(STRIP "${out}" public)
if(NOT public MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "keygen printed no public key: ${out}")
endif()
string(LENGTH "${public}" length)
if(NOT length EQUAL 128)
    message(FATAL_ERROR "the public key is ${length} digits, not 128")
endif()
file(SIZE "${dir}/update.key" size)
if(NOT size EQUAL 32)
    message(FATAL_ERROR "the private key is ${size} bytes")
endif()
execute_process(COMMAND stat -f %Lp "${dir}/update.key" OUTPUT_VARIABLE mode OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT mode STREQUAL "600")
    message(FATAL_ERROR "the private key's mode is ${mode}, not 600")
endif()
run(fail "${SIGN}" keygen "${dir}/update.key")     # never overwritten
run(ok "${SIGN}" public "${dir}/update.key")
string(STRIP "${out}" again)
if(NOT again STREQUAL public)
    message(FATAL_ERROR "public prints another key")
endif()

# A release zip as the release target makes it.
set(release "${dir}/bg3mse-0.2.0")
file(MAKE_DIRECTORY "${release}")
file(WRITE "${release}/libbg3mse.dylib" "library\n")
file(SHA256 "${release}/libbg3mse.dylib" hash)
file(WRITE "${release}/manifest.json" "{\"name\":\"bg3mse-0.2.0\",\"version\":\"0.2.0\",\"commit\":\"abc\",\"script_extender_version\":\"34.0.0.0\",\"channel\":\"Release\",\"game_build_checked\":\"4.1.1.7631656\",\"macos_minimum\":\"11.0\",\"architecture\":\"arm64\",\"files\":[{\"path\":\"libbg3mse.dylib\",\"size\":8,\"sha256\":\"${hash}\"}]}\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -DROOT=${dir} -DENTRY=bg3mse-0.2.0 -DOUTPUT=${dir}/bg3mse-0.2.0.zip
    -P "${ZIP_SCRIPT}" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "Zip.cmake failed")
endif()
file(SHA256 "${dir}/bg3mse-0.2.0.zip" zip_hash)

run(ok "${SIGN}" manifest --key "${dir}/update.key" --channel Release --out "${dir}/Manifest.json"
    --add "${dir}/bg3mse-0.2.0.zip" --min-game 4.1.1.0)
run(ok "${SIGN}" verify "${public}" "${dir}/Manifest.json")
string(JSON sequence GET "${out}" Sequence)
string(JSON url GET "${out}" Releases 0 URL)
string(JSON digest GET "${out}" Releases 0 Digest)
string(JSON min GET "${out}" Releases 0 MinGameVersion)
string(JSON checked GET "${out}" Releases 0 GameBuildChecked)
if(NOT sequence EQUAL 1 OR NOT url STREQUAL "../v0.2.0/bg3mse-0.2.0.zip" OR NOT digest STREQUAL zip_hash
        OR NOT min STREQUAL "4.1.1.0" OR NOT checked STREQUAL "4.1.1.7631656")
    message(FATAL_ERROR "the manifest is not what was asked for:\n${out}")
endif()

# The next one: sequence 2, 0.2.0 revoked; the release cannot be listed again.
run(fail "${SIGN}" manifest --key "${dir}/update.key" --channel Release --out "${dir}/Manifest2.json"
    --previous "${dir}/Manifest.json" --add "${dir}/bg3mse-0.2.0.zip")
run(ok "${SIGN}" manifest --key "${dir}/update.key" --channel Release --out "${dir}/Manifest2.json"
    --previous "${dir}/Manifest.json" --revoke 0.2.0 --notice "Read this")
run(ok "${SIGN}" verify "${public}" "${dir}/Manifest2.json")
string(JSON sequence GET "${out}" Sequence)
string(JSON revoked GET "${out}" Releases 0 Revoked)
if(NOT sequence EQUAL 2 OR NOT revoked)
    message(FATAL_ERROR "the second manifest is not what was asked for:\n${out}")
endif()

# Refusals: another channel's previous manifest, another key's, a changed one.
run(fail "${SIGN}" manifest --key "${dir}/update.key" --channel Nightly --out "${dir}/x.json" --previous "${dir}/Manifest.json")
run(ok "${SIGN}" keygen "${dir}/other.key")
string(STRIP "${out}" other)
run(fail "${SIGN}" verify "${other}" "${dir}/Manifest.json")
run(fail "${SIGN}" manifest --key "${dir}/other.key" --channel Release --out "${dir}/x.json" --previous "${dir}/Manifest.json")
file(READ "${dir}/Manifest.json" text)
string(REPLACE "4.1.1.0" "4.1.0.0" text "${text}")
file(WRITE "${dir}/Changed.json" "${text}")
run(fail "${SIGN}" verify "${public}" "${dir}/Changed.json")

file(REMOVE_RECURSE "${dir}")
message(STATUS "bg3mse-sign: keys, manifests and refusals as expected")
