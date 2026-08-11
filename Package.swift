// swift-tools-version: 6.0

import PackageDescription

let bridgeCSettings: [CSetting] = [
    .headerSearchPath("."),
    .headerSearchPath("include"),
    .headerSearchPath("aoo"),
    .headerSearchPath("deps"),
    .headerSearchPath("deps/oscpack"),
    .define("AOO_BUILD"),
    .define("AOO_NET", to: "1"),
    .define("AOO_USE_IPV6", to: "1"),
    .define("AOO_USE_OPUS", to: "0"),
    .define("AOO_SAMPLE_SIZE", to: "32"),
    .define("AOO_MAX_PACKET_SIZE", to: "4096"),
    .define("AOO_HAVE_ATOMIC_DOUBLE", to: "1"),
    .define("AOO_HAVE_ATOMIC_INT64", to: "1"),
    .define("AOO_HAVE_PTHREAD_RWLOCK", to: "1")
]

let bridgeCXXSettings: [CXXSetting] = [
    .headerSearchPath("."),
    .headerSearchPath("include"),
    .headerSearchPath("aoo"),
    .headerSearchPath("deps"),
    .headerSearchPath("deps/oscpack"),
    .define("AOO_BUILD"),
    .define("AOO_NET", to: "1"),
    .define("AOO_USE_IPV6", to: "1"),
    .define("AOO_USE_OPUS", to: "0"),
    .define("AOO_SAMPLE_SIZE", to: "32"),
    .define("AOO_MAX_PACKET_SIZE", to: "4096"),
    .define("AOO_HAVE_ATOMIC_DOUBLE", to: "1"),
    .define("AOO_HAVE_ATOMIC_INT64", to: "1"),
    .define("AOO_HAVE_PTHREAD_RWLOCK", to: "1")
]

let package = Package(
    name: "AOOApple",
    platforms: [
        .iOS(.v16),
        .macOS(.v13)
    ],
    products: [
        .library(name: "AOOApple", targets: ["AOOApple"]),
        .library(name: "AOOAppleDiscovery", targets: ["AOOAppleDiscovery"])
    ],
    targets: [
        .target(
            name: "AOOAppleBridge",
            path: ".",
            exclude: [
                "aoo/src/codec/opus.cpp",
                "deps/md5/CMakeLists.txt"
            ],
            sources: [
                "apple/Sources/AOOAppleBridge",
                "aoo/src",
                "common",
                "deps/md5",
                "deps/oscpack/osc"
            ],
            publicHeadersPath: "apple/Sources/AOOAppleBridge/include",
            cSettings: bridgeCSettings,
            cxxSettings: bridgeCXXSettings,
            linkerSettings: [
                .linkedLibrary("c++")
            ]
        ),
        .target(
            name: "AOOApple",
            dependencies: ["AOOAppleBridge"],
            path: "apple/Sources/AOOApple",
            swiftSettings: [
                .enableExperimentalFeature("StrictConcurrency")
            ]
        ),
        .target(
            name: "AOOAppleDiscovery",
            dependencies: ["AOOApple"],
            path: "apple/Sources/AOOAppleDiscovery",
            swiftSettings: [
                .enableExperimentalFeature("StrictConcurrency")
            ]
        ),
        .testTarget(
            name: "AOOAppleTests",
            dependencies: ["AOOApple"],
            path: "apple/Tests/AOOAppleTests"
        ),
        .testTarget(
            name: "AOOAppleDiscoveryTests",
            dependencies: ["AOOAppleDiscovery"],
            path: "apple/Tests/AOOAppleDiscoveryTests"
        )
    ],
    cxxLanguageStandard: .cxx17
)
