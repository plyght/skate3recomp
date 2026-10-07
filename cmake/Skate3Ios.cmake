# iOS app bundle setup: Info.plist, entitlements, ad-hoc signature and an
# unsigned-for-distribution .ipa for SideStore / AltStore / Xcode installs.
#
# The .ipa contains code recompiled from the user's own copy of the game. It
# is for installing on the user's own devices only and must not be shared.

set(SKATE3_IOS_DEPLOYMENT_TARGET "26.0" CACHE STRING "Minimum iOS version")
set(SKATE3_IOS_BUNDLE_ID "com.example.skate3recomp" CACHE STRING
    "Bundle identifier (SideStore/AltStore rewrite it to match your Apple ID)")
set(SKATE3_IOS_CODESIGN_IDENTITY "-" CACHE STRING
    "codesign identity; '-' ad-hoc signs so sideloading tools can re-sign")
option(SKATE3_IOS_EXTENDED_VIRTUAL_ADDRESSING
    "Request com.apple.developer.kernel.extended-virtual-addressing (paid developer account only)"
    OFF)

function(skate3_sign_ios_bundle target_name entitlements)
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND codesign --force --sign "${SKATE3_IOS_CODESIGN_IDENTITY}"
            --entitlements "${entitlements}" --timestamp=none
            "$<TARGET_BUNDLE_DIR:${target_name}>"
        COMMENT "Signing skate3.app (${SKATE3_IOS_CODESIGN_IDENTITY})"
        VERBATIM
    )
endfunction()

function(skate3_configure_ios_bundle target_name)
    set(SKATE3_IOS_EXTRA_ENTITLEMENTS "")
    if(SKATE3_IOS_EXTENDED_VIRTUAL_ADDRESSING)
        # With this the guest gets its full 4.5 GB contiguous layout; without
        # it the runtime falls back to the split physical view.
        string(APPEND SKATE3_IOS_EXTRA_ENTITLEMENTS
            "\t<key>com.apple.developer.kernel.extended-virtual-addressing</key>\n\t<true/>\n")
    endif()

    set(_plist "${CMAKE_CURRENT_BINARY_DIR}/ios/Info.plist")
    set(_entitlements "${CMAKE_CURRENT_BINARY_DIR}/ios/skate3.entitlements")
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/ios/Info.plist.in" "${_plist}" @ONLY)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/ios/skate3.entitlements.in" "${_entitlements}"
        @ONLY)

    set_target_properties(${target_name} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${_plist}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${SKATE3_IOS_BUNDLE_ID}"
        OUTPUT_NAME skate3
    )
    target_compile_options(${target_name} PRIVATE
        $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)

    # Sign after the bundle is assembled: deferred to the end of the directory
    # so it runs after every other POST_BUILD step that adds files to the
    # bundle (a later copy would break the sealed resources). Entitlements live
    # in the signature, which sideloading tools read back when they re-sign
    # with your profile.
    cmake_language(EVAL CODE
        "cmake_language(DEFER CALL skate3_sign_ios_bundle ${target_name} \"${_entitlements}\")")

    # skate3-ipa: Payload/skate3.app zipped, ready for SideStore/AltStore.
    set(_ipa_root "${CMAKE_CURRENT_BINARY_DIR}/ipa")
    add_custom_target(skate3-ipa
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_ipa_root}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_ipa_root}/Payload"
        COMMAND ditto "$<TARGET_BUNDLE_DIR:${target_name}>"
            "${_ipa_root}/Payload/skate3.app"
        COMMAND ${CMAKE_COMMAND} -E chdir "${_ipa_root}"
            ditto -c -k --sequesterRsrc --keepParent Payload
            "${CMAKE_CURRENT_BINARY_DIR}/Skate3Recomp.ipa"
        DEPENDS ${target_name}
        COMMENT "Packaging Skate3Recomp.ipa (personal use only - contains your game's code)"
        VERBATIM
    )
endfunction()
