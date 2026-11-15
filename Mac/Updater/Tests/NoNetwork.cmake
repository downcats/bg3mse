# A loader of a release that does not update itself (BG3MSE_SELF_UPDATE=OFF)
# has no way to the network, read from the image itself: it links no library
# that opens connections (libcurl, CFNetwork, Network, nor Foundation or the
# Objective-C runtime, through which the system's URL loading is reached),
# and imports no function that does. The loader that updates itself has
# both, so the check sees what it looks for. The installer's programs, which
# are installed in bin/ with either loader, link none of those libraries and
# import none of those functions either.
#
#   cmake -DOFFLINE=<loader>|... -DONLINE=<loader> -DTOOLS=<image>|... -P NoNetwork.cmake

string(REPLACE "|" ";" OFFLINE "${OFFLINE}")
string(REPLACE "|" ";" TOOLS "${TOOLS}")
set(denied_libraries "libcurl|CFNetwork\\.framework|Network\\.framework")
set(denied_symbols "^_(curl_[a-z_]+|socket|connect|getaddrinfo|gethostbyname2?|sendto|sendmsg|CFSocket[A-Za-z]*|CF(Read|Write)Stream[A-Za-z]*|CFHTTP[A-Za-z]*|CFURLCreateDataAndPropertiesFromResource|nw_[a-z_]+|OBJC_CLASS_\\$_NSURL(Session|Connection)[A-Za-z]*)$")

function(read_image image libraries_var symbols_var)
    execute_process(COMMAND otool -L "${image}" OUTPUT_VARIABLE libraries RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "otool -L ${image} failed")
    endif()
    execute_process(COMMAND nm -u "${image}" OUTPUT_VARIABLE symbols RESULT_VARIABLE rc)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "nm -u ${image} failed")
    endif()
    string(REPLACE "\n" ";" symbols "${symbols}")
    set(${libraries_var} "${libraries}" PARENT_SCOPE)
    set(${symbols_var} "${symbols}" PARENT_SCOPE)
endfunction()

function(network_of image libraries_regex out_var)
    read_image("${image}" libraries symbols)
    set(found "")
    string(REGEX MATCHALL "[^\n\t ]*(${libraries_regex})[^\n\t ]*" linked "${libraries}")
    list(APPEND found ${linked})
    foreach(symbol IN LISTS symbols)
        if(symbol MATCHES "${denied_symbols}")
            list(APPEND found "${symbol}")
        endif()
    endforeach()
    set(${out_var} "${found}" PARENT_SCOPE)
endfunction()

foreach(loader IN LISTS OFFLINE)
    network_of("${loader}" "${denied_libraries}|/Foundation\\.framework|libobjc" found)
    if(found)
        list(JOIN found ", " found)
        message(FATAL_ERROR "${loader}, a loader that does not update itself, has a way to the network: ${found}")
    endif()
endforeach()

network_of("${ONLINE}" "${denied_libraries}" online)
if(NOT online MATCHES "libcurl" OR NOT online MATCHES "_curl_easy_perform" OR NOT online MATCHES "NSURLSession")
    message(FATAL_ERROR "the check does not see the network in the loader that updates itself: ${online}")
endif()

foreach(tool IN LISTS TOOLS)
    network_of("${tool}" "${denied_libraries}" found)
    if(found)
        list(JOIN found ", " found)
        message(FATAL_ERROR "${tool}, installed with the loader, has a way to the network: ${found}")
    endif()
endforeach()
message(STATUS "no way to the network in the loaders that do not update themselves or the installer's programs; ${ONLINE} has one")
