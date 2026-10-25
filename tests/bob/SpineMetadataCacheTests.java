// Copyright 2020-2026 The Defold Foundation
// Copyright 2014-2020 King
// Copyright 2009-2014 Ragnar Svensson, Christian Murray
// Licensed under the Defold License version 1.0 (the "License"); you may not use
// this file except in compliance with the License.
//
// You may obtain a copy of the License, together with FAQs at
// https://www.defold.com/license
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

import com.dynamo.bob.Project;
import com.dynamo.bob.fs.DefaultFileSystem;
import com.dynamo.bob.fs.IResource;
import com.dynamo.bob.pipeline.Spine;
import com.dynamo.bob.pipeline.SpineSceneValidator;
import com.dynamo.bob.pipeline.SpineSceneValidator.SceneData;

/** Native cache regressions invoked by the Bob integration runner. */
public final class SpineMetadataCacheTests {
    private static final class Resource {
        byte[] content;
        final AtomicInteger reads = new AtomicInteger();
        final IResource resource;

        Resource(String path, byte[] content) {
            this(path, content, null, null);
        }

        Resource(String path, byte[] content, CountDownLatch readStarted, CountDownLatch releaseRead) {
            this.content = content;
            resource = (IResource) Proxy.newProxyInstance(IResource.class.getClassLoader(), new Class<?>[]{IResource.class},
                    (proxy, method, args) -> switch (method.getName()) {
                        case "getPath", "getAbsPath", "toString" -> path;
                        case "sha1" -> MessageDigest.getInstance("SHA-1").digest(this.content);
                        case "getContent" -> {
                            reads.incrementAndGet();
                            if (readStarted != null) {
                                readStarted.countDown();
                            }
                            if (releaseRead != null && !releaseRead.await(30, TimeUnit.SECONDS)) {
                                throw new AssertionError("Blocked resource read was not released");
                            }
                            yield this.content;
                        }
                        default -> throw new UnsupportedOperationException(method.getName());
                    });
        }
    }

    private static void same(Object expected, Object actual) {
        if (expected != actual) {
            throw new AssertionError("Expected the same cached metadata instance");
        }
    }

    private static void different(Object expected, Object actual) {
        if (expected == actual) {
            throw new AssertionError("Expected freshly loaded metadata");
        }
    }

    // Concurrent consumers and repeated lookups share one native load even through different resource paths.
    private static void checkConcurrentReuse(Project project, byte[] data) throws Exception {
        new SpineSceneValidator().init(project);
        int consumers = 8;
        var ready = new CountDownLatch(consumers);
        var start = new CountDownLatch(1);
        var resources = new ArrayList<Resource>();
        var futures = new ArrayList<Future<SceneData>>();
        try (var executor = Executors.newFixedThreadPool(consumers)) {
            for (int i = 0; i < consumers; ++i) {
                Resource resource = new Resource("alias-" + i + ".spinejson", data);
                resources.add(resource);
                futures.add(executor.submit(() -> {
                    ready.countDown();
                    if (!start.await(30, TimeUnit.SECONDS)) {
                        throw new AssertionError("Concurrent readers did not start");
                    }
                    return SpineSceneValidator.load(resource.resource, null);
                }));
            }
            try {
                BobAssertions.equal(true, ready.await(30, TimeUnit.SECONDS));
            } finally {
                start.countDown();
            }
            SceneData first = futures.get(0).get(30, TimeUnit.SECONDS);
            for (Future<SceneData> future : futures) {
                same(first, future.get(30, TimeUnit.SECONDS));
            }
            for (int i = 0; i < 100; ++i) {
                for (Resource resource : resources) {
                    same(first, SpineSceneValidator.load(resource.resource, null));
                }
            }
            BobAssertions.equal(1, resources.stream().mapToInt(resource -> resource.reads.get()).sum());
            System.out.println("8 concurrent consumers and 800 subsequent lookups: one skeleton load");
        }
    }

    // A blocked atlas read must not delay cached metadata or loading a different skeleton in the same build.
    private static void checkIndependentLoads(Project project, byte[] data, byte[] atlasData) throws Exception {
        new SpineSceneValidator().init(project);
        Resource source = new Resource("source.spinejson", data);
        SceneData cached = SpineSceneValidator.load(source.resource, null);
        Resource alias = new Resource("alias.spinejson", data);
        Resource other = new Resource("other.spinejson", new String(data, StandardCharsets.UTF_8)
                .replace("\"idle\"", "\"run\"").getBytes(StandardCharsets.UTF_8));
        var readStarted = new CountDownLatch(1);
        var releaseRead = new CountDownLatch(1);
        Resource atlas = new Resource("blocked.a.texturesetc", atlasData, readStarted, releaseRead);
        try (var executor = Executors.newFixedThreadPool(2)) {
            Future<SceneData> validation = executor.submit(() -> SpineSceneValidator.load(source.resource, atlas.resource));
            try {
                BobAssertions.equal(true, readStarted.await(10, TimeUnit.SECONDS));
                Future<SceneData> hit = executor.submit(() -> SpineSceneValidator.load(alias.resource, null));
                same(cached, hit.get(10, TimeUnit.SECONDS));
                BobAssertions.equal(0, alias.reads.get());

                Future<SceneData> independent = executor.submit(() -> SpineSceneValidator.load(other.resource, null));
                SceneData loaded = independent.get(10, TimeUnit.SECONDS);
                different(cached, loaded);
                BobAssertions.equal(null, loaded.getValidationError("run", ""));
            } finally {
                releaseRead.countDown();
            }
            same(cached, validation.get(10, TimeUnit.SECONDS));
            BobAssertions.equal(1, atlas.reads.get());
        }
        System.out.println("Cached metadata and another skeleton load complete while an atlas read is blocked");
    }

    // Scene validation seeds source metadata, but a cache hit must never skip validating a scene's atlas.
    private static void checkSceneReuse(Project project, byte[] data, byte[] atlasData) throws Exception {
        new SpineSceneValidator().init(project);
        Resource compiled = new Resource("compiled.spinejsonc", data);
        Resource atlas = new Resource("scene.a.texturesetc", atlasData);
        SceneData info = SpineSceneValidator.load(compiled.resource, atlas.resource);
        Resource source = new Resource("source.spinejson", data);
        same(info, SpineSceneValidator.load(source.resource, null));
        BobAssertions.equal(0, source.reads.get());
        same(info, SpineSceneValidator.load(compiled.resource, atlas.resource));
        BobAssertions.equal(2, atlas.reads.get());
        for (Resource invalidAtlas : new Resource[] {
                new Resource("missing.a.texturesetc", null),
                new Resource("invalid.a.texturesetc", new byte[] {(byte) 0xff})}) {
            try {
                SpineSceneValidator.load(source.resource, invalidAtlas.resource);
                throw new AssertionError("A cached skeleton must not hide an invalid atlas");
            } catch (Spine.SpineException expected) {
                BobAssertions.equal(true, expected.getMessage().contains(invalidAtlas.resource.getPath()));
            }
        }
        same(info, SpineSceneValidator.load(source.resource, null));
    }

    // Content changes and build boundaries cannot reuse stale metadata.
    private static void checkInvalidation(Project project, byte[] data) throws Exception {
        var lifecycle = new SpineSceneValidator();
        lifecycle.init(project);
        Resource resource = new Resource("changing.spinejson", data);
        SceneData first = SpineSceneValidator.load(resource.resource, null);
        resource.content = new String(data, StandardCharsets.UTF_8).replace("\"idle\"", "\"run\"")
                .getBytes(StandardCharsets.UTF_8);
        SceneData changed = SpineSceneValidator.load(resource.resource, null);
        different(first, changed);
        BobAssertions.equal(null, changed.getValidationError("run", ""));
        BobAssertions.equal(true, changed.getValidationError("idle", "").contains("could not be found"));

        // init must clear a previous failed build even when exit was never called.
        lifecycle.init(project);
        SceneData rebuilt = SpineSceneValidator.load(resource.resource, null);
        different(changed, rebuilt);
        lifecycle.exit(project);
        different(rebuilt, SpineSceneValidator.load(resource.resource, null));
    }

    // Failed parses are retried and a corrected skeleton recovers without restarting the project.
    private static void checkFailureRecovery(byte[] data) throws Exception {
        Resource resource = new Resource("broken.spinejson", "{broken".getBytes(StandardCharsets.UTF_8));
        for (int i = 0; i < 2; ++i) {
            try {
                SpineSceneValidator.load(resource.resource, null);
                throw new AssertionError("Malformed skeleton must fail");
            } catch (Spine.SpineException expected) {
                BobAssertions.equal(true, expected.getMessage().contains("broken.spinejson"));
            }
        }
        BobAssertions.equal(2, resource.reads.get());
        resource.content = data;
        BobAssertions.equal(null, SpineSceneValidator.load(resource.resource, null).getValidationError("idle", ""));
    }

    // Binary source/compiled aliases share metadata, while the same bytes with a JSON hint must still be parsed as JSON.
    private static void checkBinaryFormat(byte[] binary) throws Exception {
        Resource source = new Resource("source.SkEl", binary);
        Resource compiled = new Resource("compiled.skelc", binary);
        same(SpineSceneValidator.load(compiled.resource, null),
                SpineSceneValidator.load(source.resource, null));
        BobAssertions.equal(0, source.reads.get());
        try {
            SpineSceneValidator.load(new Resource("wrong.spinejson", binary).resource, null);
            throw new AssertionError("Metadata must not be reused across input formats");
        } catch (Spine.SpineException expected) {
            BobAssertions.equal(true, expected.getMessage().contains("wrong.spinejson"));
        }
    }

    public static void main(String[] args) throws Exception {
        Path root = Path.of(args[0]);
        try (var paths = Files.list(root.resolve("build/plugins/defold-spine/plugins/lib"))) {
            System.setProperty("jna.library.path", paths.findFirst().orElseThrow().toString());
        }
        byte[] data = Files.readAllBytes(root.resolve("data.spinejson"));
        byte[] atlas = Files.readAllBytes(root.resolve("build/default/corrupted/_assets/shared/test.a.texturesetc"));
        Project project = new Project(new DefaultFileSystem());
        try {
            checkConcurrentReuse(project, data);
            checkIndependentLoads(project, data, atlas);
            checkSceneReuse(project, data, atlas);
            checkInvalidation(project, data);
            checkFailureRecovery(data);
            checkBinaryFormat(Files.readAllBytes(root.resolve("corrupted/_assets/cases/00_valid_control/case.skel")));
        } finally {
            new SpineSceneValidator().exit(project);
        }
    }
}
