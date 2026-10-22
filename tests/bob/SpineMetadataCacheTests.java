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
            this.content = content;
            resource = (IResource) Proxy.newProxyInstance(IResource.class.getClassLoader(), new Class<?>[]{IResource.class},
                    (proxy, method, args) -> switch (method.getName()) {
                        case "getPath", "getAbsPath", "toString" -> path;
                        case "sha1" -> MessageDigest.getInstance("SHA-1").digest(this.content);
                        case "getContent" -> {
                            reads.incrementAndGet();
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
                    return SpineSceneValidator.load(project, resource.resource, null);
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
                    same(first, SpineSceneValidator.load(project, resource.resource, null));
                }
            }
            BobAssertions.equal(1, resources.stream().mapToInt(resource -> resource.reads.get()).sum());
            System.out.println("8 concurrent consumers and 800 subsequent lookups: one skeleton load");
        }
    }

    // Scene validation seeds source metadata, but a cache hit must never skip validating a scene's atlas.
    private static void checkSceneReuse(Project project, byte[] data, byte[] atlasData) throws Exception {
        new SpineSceneValidator().init(project);
        Resource compiled = new Resource("compiled.spinejsonc", data);
        Resource atlas = new Resource("scene.a.texturesetc", atlasData);
        SceneData info = SpineSceneValidator.load(project, compiled.resource, atlas.resource);
        Resource source = new Resource("source.spinejson", data);
        same(info, SpineSceneValidator.load(project, source.resource, null));
        BobAssertions.equal(0, source.reads.get());
        same(info, SpineSceneValidator.load(project, compiled.resource, atlas.resource));
        BobAssertions.equal(2, atlas.reads.get());
        try {
            SpineSceneValidator.load(project, source.resource, new Resource("missing.a.texturesetc", null).resource);
            throw new AssertionError("A cached skeleton must not hide an unreadable atlas");
        } catch (Spine.SpineException expected) {
            BobAssertions.equal(true, expected.getMessage().contains("missing.a.texturesetc"));
        }
    }

    // Changes at the same path, separate projects and a new build cannot reuse stale metadata.
    private static void checkInvalidation(Project project, byte[] data) throws Exception {
        var lifecycle = new SpineSceneValidator();
        lifecycle.init(project);
        Resource resource = new Resource("changing.spinejson", data);
        SceneData first = SpineSceneValidator.load(project, resource.resource, null);
        resource.content = new String(data, StandardCharsets.UTF_8).replace("\"idle\"", "\"run\"")
                .getBytes(StandardCharsets.UTF_8);
        SceneData changed = SpineSceneValidator.load(project, resource.resource, null);
        different(first, changed);
        BobAssertions.equal(null, changed.getValidationError("run", ""));
        BobAssertions.equal(true, changed.getValidationError("idle", "").contains("could not be found"));

        // init must clear a previous failed build even when exit was never called.
        lifecycle.init(project);
        SceneData rebuilt = SpineSceneValidator.load(project, resource.resource, null);
        different(changed, rebuilt);
        Project other = new Project(new DefaultFileSystem());
        different(rebuilt, SpineSceneValidator.load(other, resource.resource, null));
        lifecycle.exit(other);
        lifecycle.exit(project);
        different(rebuilt, SpineSceneValidator.load(project, resource.resource, null));
    }

    // Failed parses are retried and a corrected skeleton recovers without restarting the project.
    private static void checkFailureRecovery(Project project, byte[] data) throws Exception {
        Resource resource = new Resource("broken.spinejson", "{broken".getBytes(StandardCharsets.UTF_8));
        for (int i = 0; i < 2; ++i) {
            try {
                SpineSceneValidator.load(project, resource.resource, null);
                throw new AssertionError("Malformed skeleton must fail");
            } catch (Spine.SpineException expected) {
                BobAssertions.equal(true, expected.getMessage().contains("broken.spinejson"));
            }
        }
        BobAssertions.equal(2, resource.reads.get());
        resource.content = data;
        BobAssertions.equal(null, SpineSceneValidator.load(project, resource.resource, null).getValidationError("idle", ""));
    }

    // Binary source/compiled aliases share metadata, while the same bytes with a JSON hint must still be parsed as JSON.
    private static void checkBinaryFormat(Project project, byte[] binary) throws Exception {
        Resource source = new Resource("source.SkEl", binary);
        Resource compiled = new Resource("compiled.skelc", binary);
        same(SpineSceneValidator.load(project, compiled.resource, null),
                SpineSceneValidator.load(project, source.resource, null));
        BobAssertions.equal(0, source.reads.get());
        try {
            SpineSceneValidator.load(project, new Resource("wrong.spinejson", binary).resource, null);
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
            checkSceneReuse(project, data, atlas);
            checkInvalidation(project, data);
            checkFailureRecovery(project, data);
            checkBinaryFormat(project, Files.readAllBytes(root.resolve("corrupted/_assets/cases/00_valid_control/case.skel")));
        } finally {
            new SpineSceneValidator().exit(project);
        }
    }
}
