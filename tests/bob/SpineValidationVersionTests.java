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

import java.util.Arrays;
import java.util.HashSet;
import java.util.Set;

import com.dynamo.bob.Bob;
import com.dynamo.bob.Progress;
import com.dynamo.bob.TaskResult;
import com.dynamo.bob.pipeline.SpineSceneValidator;

/** Checks incremental task outcomes without relying on output timestamps. */
public final class SpineValidationVersionTests {
    // All three validators must rebuild on a version change and skip again with the same version.
    public static void main(String[] args) throws Exception {
        if (args[0].equals("--cache-key")) {
            // Read the field reflectively so this test does not inline the original version.
            System.out.println(SpineSceneValidator.class.getField("CACHE_KEY").get(null));
            return;
        }
        TaskResult.Result expected = TaskResult.Result.valueOf(args[0]);
        var result = Bob.invoke(null, Progress.discarding(), null, Arrays.copyOfRange(args, 1, args.length));
        BobAssertions.equal(true, result.success);
        Set<String> remaining = new HashSet<>(Set.of("SpineScene", "SpineModel", "Gui"));
        for (TaskResult task : result.taskResults) {
            if (remaining.remove(task.getTask().getName())) {
                if (task.getResult() != expected) {
                    throw new AssertionError(task.getTask().getName() + ": expected " + expected + ", got " + task.getResult());
                }
            }
        }
        BobAssertions.equal(Set.of(), remaining);
    }
}
