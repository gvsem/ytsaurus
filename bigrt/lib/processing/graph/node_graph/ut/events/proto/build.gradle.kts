import com.google.protobuf.gradle.*

val buildProtoDir = File("${buildDir}", "__proto__")

plugins {
    id("java-library")
    id("com.google.protobuf") version "0.8.19"
}


repositories {
    mavenCentral()
}

java {
    withSourcesJar()
    withJavadocJar()
}

dependencies {
    api("com.google.protobuf:protobuf-java:3.22.5")
    api(project(":bigrt:lib:events:proto"))

    protobuf(files(buildProtoDir))
}

protobuf {
    protoc {
        // Download from repositories
        artifact = "com.google.protobuf:protoc:3.22.5"
    }

}

val prepareProto = tasks.register<Copy>("prepareProto") {
    from(rootDir) {
        include("bigrt/lib/processing/graph/node_graph/ut/events/proto/test_event.proto")
        include("bigrt/lib/processing/graph/node_graph/ut/events/proto/types.proto")
    }
    into(buildProtoDir)
}

afterEvaluate {
    tasks.getByName("extractProto").dependsOn(prepareProto)
}
