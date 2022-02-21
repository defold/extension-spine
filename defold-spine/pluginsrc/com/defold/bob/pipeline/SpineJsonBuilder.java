package com.dynamo.bob.pipeline;

import com.dynamo.bob.BuilderParams;
import com.dynamo.bob.CopyBuilder;

@BuilderParams(name="SpineJsonFile", inExts=".spinejson", outExt=".spinejsonc")
public class SpineJsonBuilder extends CopyBuilder {}
