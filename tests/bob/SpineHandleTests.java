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

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;

import org.codehaus.jackson.map.ObjectMapper;

import com.dynamo.bob.pipeline.Spine;

/** Ownership and error isolation checks against the host's native Spine plugin. */
public final class SpineHandleTests {
    // A later atlas or file-read failure must replace an earlier skeleton error, and successful loads clear it.
    private static void checkLastError(Path root, byte[] data, byte[] atlas) throws Exception {
        try (Spine.SpinePointer handle = Spine.SPINE_LoadFileFromBuffer(new byte[] {0}, "truncated.skel")) {
            throw new AssertionError("Truncated skeleton must fail");
        } catch (Spine.SpineException error) {
            BobAssertions.equal(true, error.getMessage().contains("binary skeleton is truncated"));
        }
        try (Spine.SpinePointer handle = Spine.SPINE_LoadFileFromBuffer(data, "valid.spinejson", new byte[] {(byte) 0xff}, "invalid.texturesetc")) {
            throw new AssertionError("Invalid atlas must fail");
        } catch (Spine.SpineException error) {
            BobAssertions.equal(true, error.getMessage().contains("Failed to load atlas from 'invalid.texturesetc'"));
            BobAssertions.equal(false, error.getMessage().contains("binary skeleton is truncated"));
        }
        for (String[] paths : new String[][] {
                {"missing.spinejson", "build/default/corrupted/_assets/shared/test.a.texturesetc", "missing.spinejson"},
                {"data.spinejson", "missing.texturesetc", "missing.texturesetc"}}) {
            try (Spine.SpinePointer handle = new Spine.SpinePointer(Spine.SPINE_LoadFromPath(
                    root.resolve(paths[0]).toString(), root.resolve(paths[1]).toString()))) {
                BobAssertions.equal(null, handle.getPointer());
                BobAssertions.equal("Failed to read file '" + root.resolve(paths[2]) + "'", Spine.SPINE_GetLastError());
            }
        }
        try (Spine.SpinePointer handle = Spine.SPINE_LoadFileFromBuffer(data, "valid.spinejson", atlas, "valid.texturesetc")) {
            BobAssertions.equal("", Spine.SPINE_GetLastError());
        }
    }

    // Explicit close and the legacy finalizer must never destroy the same native handle twice.
    private static void checkClose(byte[] data) throws Exception {
        Spine.SpinePointer handle = Spine.SPINE_LoadFileFromBuffer(data, "close.spinejson");
        BobAssertions.equal(2, Spine.SPINE_GetAnimations(handle).length);
        handle.close();
        handle.close();
        handle.finalize();
        BobAssertions.equal(null, handle.getPointer());

        Spine.SpinePointer finalized = Spine.SPINE_LoadFileFromBuffer(data, "finalize.spinejson");
        finalized.finalize();
        finalized.close();
        BobAssertions.equal(null, finalized.getPointer());
        new Spine.SpinePointer().close();
    }

    // Exceptional exits from metadata extraction must release the handle without a caller-side finally protocol.
    private static void checkExceptionalClose(byte[] data) throws Exception {
        Spine.SpinePointer handle = Spine.SPINE_LoadFileFromBuffer(data, "exception.spinejson");
        RuntimeException expected = new RuntimeException("metadata extraction failed");
        try (handle) {
            throw expected;
        } catch (RuntimeException error) {
            if (error != expected) {
                throw error;
            }
        }
        BobAssertions.equal(null, handle.getPointer());
        handle.close();
    }

    // Both wrapper overloads must keep each native diagnostic attached to the load that produced it.
    private static void checkConcurrentErrors(byte[] data, byte[] atlas) throws Exception {
        String skeleton = new ObjectMapper().readTree(data).get("skeleton").toString();
        int workers = 8;
        CountDownLatch ready = new CountDownLatch(workers);
        CountDownLatch start = new CountDownLatch(1);
        ArrayList<Future<?>> futures = new ArrayList<>();
        try (var executor = Executors.newFixedThreadPool(workers)) {
            for (int i = 0; i < workers; ++i) {
                String marker = "missing-parent-" + i;
                byte[] invalid = ("{\"skeleton\":" + skeleton + ",\"bones\":[{\"name\":\"child\",\"parent\":\"" + marker + "\"}]}")
                        .getBytes(StandardCharsets.UTF_8);
                futures.add(executor.submit(() -> {
                    ready.countDown();
                    if (!start.await(30, TimeUnit.SECONDS)) {
                        throw new AssertionError("Concurrent loaders did not start");
                    }
                    for (int attempt = 0; attempt < 20; ++attempt) {
                        try (Spine.SpinePointer handle = attempt % 2 == 0
                                ? Spine.SPINE_LoadFileFromBuffer(invalid, "invalid.spinejson")
                                : Spine.SPINE_LoadFileFromBuffer(invalid, "invalid.spinejson", atlas, "atlas.texturesetc")) {
                            throw new AssertionError("Invalid parent must fail loading");
                        } catch (Spine.SpineException error) {
                            if (!error.getMessage().contains(marker)) {
                                throw new AssertionError("Expected " + marker + " in: " + error.getMessage());
                            }
                        }
                    }
                    return null;
                }));
            }
            try {
                BobAssertions.equal(true, ready.await(30, TimeUnit.SECONDS));
            } finally {
                start.countDown();
            }
            for (Future<?> future : futures) {
                future.get(30, TimeUnit.SECONDS);
            }
        }
    }

    public static void main(String[] args) throws Exception {
        Path root = Path.of(args[0]);
        try (var paths = Files.list(root.resolve("build/plugins/defold-spine/plugins/lib"))) {
            System.setProperty("jna.library.path", paths.findFirst().orElseThrow().toString());
        }
        byte[] data = Files.readAllBytes(root.resolve("data.spinejson"));
        byte[] atlas = Files.readAllBytes(root.resolve("build/default/corrupted/_assets/shared/test.a.texturesetc"));
        checkLastError(root, data, atlas);
        checkClose(data);
        checkExceptionalClose(data);
        checkConcurrentErrors(data, atlas);
        System.out.println("Native handle ownership and concurrent diagnostics verified");
    }
}
