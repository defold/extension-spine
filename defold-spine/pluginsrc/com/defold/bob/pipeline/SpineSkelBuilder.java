package com.dynamo.bob.pipeline;

import com.dynamo.bob.BuilderParams;
import com.dynamo.bob.CopyBuilder;

@BuilderParams(name="SpineSkelFile", inExts=".skel", outExt=".skelc")
public class SpineSkelBuilder extends CopyBuilder {}
