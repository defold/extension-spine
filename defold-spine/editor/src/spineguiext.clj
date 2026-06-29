;; Copyright 2020 The Defold Foundation
;; Licensed under the Defold License version 1.0 (the "License"); you may not use
;; this file except in compliance with the License.
;;
;; You may obtain a copy of the License, together with FAQs at
;; https://www.defold.com/license
;;
;; Unless required by applicable law or agreed to in writing, software distributed
;; under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
;; CONDITIONS OF ANY KIND, either express or implied. See the License for the
;; specific language governing permissions and limitations under the License.

(ns editor.spineguiext
  (:require [clojure.string :as str]
            [dynamo.graph :as g]
            [editor.defold-project :as project]
            [editor.editor-extensions.graph :as ext-graph]
            [editor.editor-extensions.node-types :as node-types]
            [editor.geom :as geom]
            [editor.gl.texture]
            [editor.graph-util :as gu]
            [editor.gui :as gui]
            [editor.outline :as outline]
            [editor.properties :as properties]
            [editor.protobuf :as protobuf]
            [editor.resource :as resource]
            [editor.spineext :as spineext]
            [editor.types :as types]
            [editor.util :as util]
            [util.coll :as coll]
            [util.murmur :as murmur])
  (:import [com.dynamo.gamesys.proto Gui$NodeDesc Gui$NodeDesc$ClippingMode]
           [editor.gl.texture TextureLifecycle]))

(set! *warn-on-reflection* true)

; Plugin functions (from Spine.java)

;; (defn- debug-cls [^Class cls]
;;   (doseq [m (.getMethods cls)]
;;     (prn (.toString m))
;;     (println "Method Name: " (.getName m) "(" (.getParameterTypes m) ")")
;;     (println "Return Type: " (.getReturnType m) "\n")))

; More about JNA + Clojure
; https://nakkaya.com/2009/11/16/java-native-access-from-clojure/


;;//////////////////////////////////////////////////////////////////////////////////////////////

;; Spine nodes

(def ^:private validate-spine-scene (partial gui/validate-required-gui-resource "spine scene '%s' does not exist in the scene" :spine-scene))

(defn- spine-scene-names [basic-gui-scene-info]
  (get-in basic-gui-scene-info [:gui-resource-kind-names :spine-scene]))

(defn- spine-scene-basic-info [basic-gui-scene-info]
  (get-in basic-gui-scene-info [:gui-resource-kind-basic-info :spine-scene]))

(defn- spine-scene-costly-info [costly-gui-scene-info]
  (get-in costly-gui-scene-info [:gui-resource-kind-costly-info :spine-scene]))

(defn- validate-spine-default-animation [node-id spine-scene-names spine-anim-ids spine-default-animation spine-scene]
  (when-not (g/error? (validate-spine-scene node-id spine-scene-names spine-scene))
    (gui/validate-optional-gui-resource "animation '%s' could not be found in the specified spine scene" :spine-default-animation node-id spine-anim-ids spine-default-animation)))

(defn- validate-spine-skin [node-id spine-scene-names spine-skin-ids spine-skin spine-scene]
  (when-not (g/error? (validate-spine-scene node-id spine-scene-names spine-scene))
    (spineext/validate-skin node-id :spine-skin spine-skin-ids spine-skin)))

(defn- update-vertices! [handle skin anim dt world-transform color]
  (when (some? handle)
    (when-not (str/blank? skin)
      (spineext/plugin-set-skin handle skin))
    (when-not (str/blank? anim)
      (spineext/plugin-set-animation handle anim))
    (spineext/plugin-update-vertices handle dt world-transform color false)))

(defn- produce-vertices [handle skin anim dt world-transform color]
  (if (some? handle)
    (let [_ (update-vertices! handle skin anim dt world-transform color)
          vb-data (spineext/plugin-get-vertex-buffer-data handle)] ; list of SpineVertex
      (into [] (spineext/transform-vertices-as-vec vb-data))) ; unpacked into lists of lists [[x y z u v r g b a page_index]])
    []))

(g/defnk produce-spine-updatable [_node-id spine-data-handle spine-default-animation spine-skin]
  (when (and (some? spine-data-handle)
             spine-default-animation
             spine-skin)
    {:node-id _node-id
     :name "Spine GUI Updater"
     :update-fn (fn [state {:keys [dt]}]
                  (update-vertices! spine-data-handle spine-skin spine-default-animation dt geom/Identity4d spineext/identity-color)
                  state)
     :initial-state {}}))

(defn- renderable->vertices [renderable]
  (let [handle (spineext/renderable->handle renderable)
        world-transform (:world-transform renderable)
        per-node-user-data (:user-data renderable)
        skin (:spine-skin per-node-user-data)
        anim (:spine-default-animation per-node-user-data)
        color (:color per-node-user-data)]
    (produce-vertices handle skin anim 0.0 world-transform color)))

(defn- gen-vb [_user-data renderables]
  (let [vertices (into [] (mapcat renderable->vertices) renderables)
        vb-out (spineext/generate-vertex-buffer vertices)]
    vb-out))

(defn- aabb->rect-lines [aabb]
  (let [minp (types/min-p aabb)
        maxp (types/max-p aabb)
        [x0 y0 _] (types/Point3d->Vec3 minp)
        [x1 y1 _] (types/Point3d->Vec3 maxp)]
    [[x0 y0 0] [x1 y0 0]
     [x1 y0 0] [x1 y1 0]
     [x1 y1 0] [x0 y1 0]
     [x0 y1 0] [x0 y0 0]]))

(def ^:private custom-property-pb-type->value-field
  {:type-boolean :boolean
   :type-hash :string
   :type-number :number
   :type-quat :quat
   :type-string :string
   :type-vector3 :vector3
   :type-vector4 :vector4})

(defn- custom-property-info->pb [{:keys [id protobuf-type]} value]
  (let [value-field (custom-property-pb-type->value-field protobuf-type)]
    {:id id
     :type protobuf-type
     value-field value}))

(def ^:private legacy-spine-prop-keys
  [:spine-create-bones
   :spine-default-animation
   :spine-scene
   :spine-skin])

(def ^:private legacy-spine-pb-field-indices
  (coll/pair-map-by gui/prop-key->pb-field-index legacy-spine-prop-keys))

(defn- strip-legacy-spine-overridden-fields [node-desc]
  (protobuf/assign-repeated node-desc :overridden-fields
                            (into []
                                  (remove legacy-spine-pb-field-indices)
                                  (:overridden-fields node-desc))))

(g/defnk produce-spine-node-msg
  [visual-base-node-msg
   ^:raw spine-scene
   ^:raw spine-default-animation
   ^:raw spine-skin
   ^:raw spine-create-bones
   ^:raw clipping-mode
   ^:raw clipping-visible
   ^:raw clipping-inverted]
  (assoc visual-base-node-msg
         :spine-scene spine-scene
         :spine-default-animation spine-default-animation
         :spine-skin spine-skin
         :spine-create-bones spine-create-bones
         :clipping-mode clipping-mode
         :clipping-visible clipping-visible
         :clipping-inverted clipping-inverted
         :size-mode :size-mode-auto))

(g/defnode SpineNode
  (inherits gui/VisualNode)

  (property spine-scene g/Str (default "")
            (static custom-property {:id "spine_scene"
                                     :protobuf-type :type-string})
            (dynamic edit-type (g/fnk [basic-gui-scene-info]
                                 (gui/wrap-layout-property-edit-type spine-scene (gui/required-gui-resource-choicebox (spine-scene-names basic-gui-scene-info)))))
            (dynamic ext-edit-type (g/constantly {:type g/Str}))
            (dynamic error (g/fnk [_node-id basic-gui-scene-info spine-scene]
                             (validate-spine-scene _node-id (spine-scene-names basic-gui-scene-info) spine-scene)))
            (dynamic label (g/constantly "Spine Scene"))
            (value (gui/layout-property-getter spine-scene))
            (set (gui/layout-property-setter spine-scene)))
  (property spine-default-animation g/Str (default "")
            (static custom-property {:id "spine_default_animation"
                                     :protobuf-type :type-string})
            (dynamic edit-type (g/fnk [spine-anim-ids]
                                 (gui/wrap-layout-property-edit-type spine-default-animation (gui/optional-gui-resource-choicebox spine-anim-ids))))
            (dynamic ext-edit-type (g/constantly {:type g/Str}))
            (dynamic error (g/fnk [_node-id basic-gui-scene-info spine-anim-ids spine-default-animation spine-scene]
                             (validate-spine-default-animation _node-id (spine-scene-names basic-gui-scene-info) spine-anim-ids spine-default-animation spine-scene)))
            (dynamic label (g/constantly "Default Animation"))
            (value (gui/layout-property-getter spine-default-animation))
            (set (gui/layout-property-setter spine-default-animation)))
  (property spine-skin g/Str (default "")
            (static custom-property {:id "spine_skin"
                                     :protobuf-type :type-string})
            (dynamic edit-type (g/fnk [spine-skin-ids]
                                 (gui/wrap-layout-property-edit-type spine-skin (spineext/->skin-choicebox spine-skin-ids))))
            (dynamic ext-edit-type (g/constantly {:type g/Str}))
            (dynamic error (g/fnk [_node-id basic-gui-scene-info spine-skin spine-skin-ids spine-scene]
                             (validate-spine-skin _node-id (spine-scene-names basic-gui-scene-info) spine-skin-ids spine-skin spine-scene)))
            (dynamic label (g/constantly "Skin"))
            (value (gui/layout-property-getter spine-skin))
            (set (gui/layout-property-setter spine-skin)))
  (property spine-create-bones g/Bool (default false)
            (static custom-property {:id "spine_create_bones"
                                     :protobuf-type :type-boolean})
            (dynamic edit-type (gui/layout-property-edit-type spine-create-bones {:type g/Bool}))
            (dynamic label (g/constantly "Create Bones"))
            (value (gui/layout-property-getter spine-create-bones))
            (set (gui/layout-property-setter spine-create-bones)))

  (property clipping-mode g/Keyword (default (protobuf/default Gui$NodeDesc :clipping-mode))
            (dynamic edit-type (gui/layout-property-edit-type clipping-mode (properties/->pb-choicebox Gui$NodeDesc$ClippingMode)))
            (value (gui/layout-property-getter clipping-mode))
            (set (gui/layout-property-setter clipping-mode)))
  (property clipping-visible g/Bool (default (protobuf/default Gui$NodeDesc :clipping-visible))
            (dynamic edit-type (gui/layout-property-edit-type clipping-visible {:type g/Bool}))
            (value (gui/layout-property-getter clipping-visible))
            (set (gui/layout-property-setter clipping-visible)))
  (property clipping-inverted g/Bool (default (protobuf/default Gui$NodeDesc :clipping-inverted))
            (dynamic edit-type (gui/layout-property-edit-type clipping-inverted {:type g/Bool}))
            (value (gui/layout-property-getter clipping-inverted))
            (set (gui/layout-property-setter clipping-inverted)))
  (display-order (into gui/base-display-order
                       [:spine-scene :spine-default-animation :spine-skin :spine-create-bones :color :alpha :inherit-alpha :layer :blend-mode :pivot :x-anchor :y-anchor
                        :adjust-mode :clipping :visible-clipper :inverted-clipper]))

  (output node-msg g/Any :cached produce-spine-node-msg)
  (output spine-anim-ids gui/GuiResourceNames
          (g/fnk [basic-gui-scene-info spine-scene]
            (let [basic-info (spine-scene-basic-info basic-gui-scene-info)]
              (:spine-anim-ids (or (get basic-info spine-scene)
                                   (get basic-info ""))))))
  (output spine-skin-ids gui/GuiResourceNames
          (g/fnk [basic-gui-scene-info spine-scene]
            (let [basic-info (spine-scene-basic-info basic-gui-scene-info)]
              (:spine-skin-ids (or (get basic-info spine-scene)
                                   (get basic-info ""))))))
  (output spine-scene-scene g/Any
          (g/fnk [costly-gui-scene-info spine-scene]
            (let [costly-info (spine-scene-costly-info costly-gui-scene-info)]
              (:spine-scene-scene (or (get costly-info spine-scene)
                                      (get costly-info ""))))))
  (output spine-scene-bones g/Any
          (g/fnk [costly-gui-scene-info spine-scene]
            (let [costly-info (spine-scene-costly-info costly-gui-scene-info)]
              (:spine-bones (or (get costly-info spine-scene)
                                (get costly-info ""))))))
  (output spine-scene-pb g/Any
          (g/fnk [costly-gui-scene-info spine-scene]
            (let [costly-info (spine-scene-costly-info costly-gui-scene-info)]
              (:spine-scene-pb (or (get costly-info spine-scene)
                                   (get costly-info ""))))))

  (output spine-data-handle g/Any :cached
          (g/fnk [_node-id costly-gui-scene-info spine-scene spine-default-animation spine-skin spine-anim-ids spine-skin-ids]
            (let [costly-info (spine-scene-costly-info costly-gui-scene-info)]
              (when-let [spine-info (or (get costly-info spine-scene)
                                         (get costly-info ""))]
                (spineext/make-spine-data-handle
                  _node-id
                  (:spine-json-resource spine-info)
                  (:spine-json-content spine-info)
                  (:atlas-resource spine-info)
                  (:texture-set-pb spine-info)
                  (if (str/blank? spine-default-animation)
                    (first spine-anim-ids)
                    spine-default-animation)
                  (if (str/blank? spine-skin)
                    (first spine-skin-ids)
                    spine-skin))))))

  (output aabb g/Any
          (g/fnk [spine-data-handle]
            (if spine-data-handle
              (spineext/handle->aabb spine-data-handle)
              geom/empty-bounding-box)))

  ; Overloaded outputs from VisualNode
  (output scene-updatable g/Any :cached produce-spine-updatable)
  (output gpu-texture TextureLifecycle (g/constantly nil))
  (output scene-renderable-user-data g/Any :cached (g/fnk [aabb spine-scene-scene spine-data-handle spine-skin spine-default-animation color+alpha clipping-mode clipping-inverted clipping-visible]
                                                          (let [lines (aabb->rect-lines aabb)
                                                                user-data (assoc (get-in spine-scene-scene [:renderable :user-data])
                                                                                 :color color+alpha
                                                                                 :renderable-tags #{:gui-spine}
                                                                                 :gen-vb gen-vb
                                                                                 :spine-data-handle spine-data-handle
                                                                                 :spine-skin spine-skin
                                                                                 :spine-default-animation spine-default-animation
                                                                                 :line-data lines)]
                                                            (cond-> user-data
                                                              (not= :clipping-mode-none clipping-mode)
                                                              (assoc :clipping {:mode clipping-mode :inverted clipping-inverted :visible clipping-visible})))))

  (output own-build-errors g/Any
          (g/fnk [_node-id basic-gui-scene-info build-errors-visual-node spine-anim-ids spine-default-animation spine-skin-ids spine-skin spine-scene]
            (let [spine-scene-names (spine-scene-names basic-gui-scene-info)]
              (g/package-errors
                _node-id
                build-errors-visual-node
                (validate-spine-scene _node-id spine-scene-names spine-scene)
                (validate-spine-default-animation _node-id spine-scene-names spine-anim-ids spine-default-animation spine-scene)
                (validate-spine-skin _node-id spine-scene-names spine-skin-ids spine-skin spine-scene))))))

(defmethod gui/update-gui-resource-reference [::SpineNode :spine-scene]
  [_ evaluation-context node-id old-name new-name]
  (gui/update-basic-gui-resource-reference evaluation-context node-id :spine-scene old-name new-name))

;;//////////////////////////////////////////////////////////////////////////////////////////////

(g/defnk produce-spine-scene-basic-info [name spine-data-handle spine-anim-ids spine-skins spine-scene]
  ;; If the referenced spine-scene-resource is missing, we don't return an entry.
  ;; This will cause every usage to fall back on the no-spine-scene entry for "".
  ;; NOTE: the no-spine-scene entry uses an instance of SpineSceneNode with an empty name.
  ;; It does not have any data, but it should still return an entry.
  (when (or (and (= "" name) (nil? spine-scene))
            (every? some? [spine-data-handle spine-anim-ids spine-skins]))
    {name {:spine-anim-ids (into (sorted-set) spine-anim-ids)
           :spine-skin-ids (into (sorted-set) spine-skins)}}))

(g/defnk produce-spine-scene-costly-info [_node-id name spine-data-handle spine-scene spine-json-resource atlas-resource texture-set-pb spine-json-content spine-bones spine-scene-pb spine-scene-scene spine-skin-aabbs]
  ;; If the referenced spine-scene-resource is missing, we don't return an entry.
  ;; This will cause every usage to fall back on the no-spine-scene entry for "".
  ;; NOTE: the no-spine-scene entry uses an instance of SpineSceneNode with an empty name.
  ;; It does not have any data, but it should still return an entry.
  (when (or (and (= "" name) (nil? spine-scene))
            (every? some? [spine-data-handle spine-scene-pb spine-scene-scene]))
    {name {:spine-data-handle spine-data-handle
           :spine-json-resource spine-json-resource
           :atlas-resource atlas-resource
           :texture-set-pb texture-set-pb
           :spine-json-content spine-json-content
           :spine-bones spine-bones
           :spine-skin-aabbs spine-skin-aabbs
           :spine-scene-pb spine-scene-pb
           :spine-scene-scene spine-scene-scene}}))

(g/defnode SpineSceneNode
  (inherits outline/OutlineNode)
  (property name g/Str
            (dynamic error (g/fnk [_node-id name name-counts] (gui/prop-unique-id-error _node-id :name name name-counts "Name")))
            (set (partial gui/update-gui-resource-references :spine-scene)))
  (property spine-scene resource/Resource
            (value (gu/passthrough spine-scene-resource))
            (set (fn [evaluation-context self old-value new-value]
                   (project/resource-setter
                    evaluation-context self old-value new-value
                    [:resource :spine-scene-resource]
                    [:build-targets :dep-build-targets]
                    [:scene :spine-scene-scene]
                    [:spine-json-resource :spine-json-resource]
                    [:atlas-resource :atlas-resource]
                    [:texture-set-pb :texture-set-pb]
                    [:spine-json-content :spine-json-content]
                    [:skin-aabbs :spine-skin-aabbs]
                    [:spine-scene-pb :spine-scene-pb]
                    [:spine-data-handle :spine-data-handle]
                    [:animations :spine-anim-ids]
                    [:skins :spine-skins]
                    [:bones :spine-bones])))
            (dynamic error (g/fnk [_node-id spine-scene]
                                  (gui/prop-resource-error _node-id :spine-scene spine-scene "Spine Scene")))
            (dynamic edit-type (g/constantly
                                {:type resource/Resource
                                 :ext ["spinescene"]})))

  (input dep-build-targets g/Any)
  (input name-counts gui/NameCounts)
  (input spine-scene-resource resource/Resource)
  (input spine-json-resource resource/Resource :substitute nil)
  (input atlas-resource resource/Resource :substitute nil)
  (input texture-set-pb g/Any :substitute (constantly nil))
  (input spine-json-content g/Any :substitute (constantly nil))

  (input spine-data-handle g/Any :substitute nil) ; The c++ pointer
  (output spine-data-handle g/Any (gu/passthrough spine-data-handle))

  (input spine-anim-ids g/Any :substitute (constantly nil))
  (input spine-skins g/Any :substitute (constantly nil))
  (input spine-bones g/Any :substitute (constantly nil))

  (input spine-scene-scene g/Any :substitute (constantly nil))
  (input spine-skin-aabbs g/Any :substitute (constantly nil))
  (input spine-scene-pb g/Any :substitute (constantly nil))

  (output dep-build-targets g/Any (gu/passthrough dep-build-targets))
  (output node-outline outline/OutlineData :cached (g/fnk [_node-id name spine-scene-resource build-errors]
                                                     (cond-> {:node-id _node-id
                                                              :node-outline-key name
                                                              :label name
                                                              :icon spineext/spine-scene-icon
                                                              :outline-error? (g/error-fatal? build-errors)}

                                                             (resource/resource? spine-scene-resource)
                                                             (assoc :link spine-scene-resource :outline-show-link? true))))
  (output pb-msg g/Any (g/fnk [name spine-scene]
                              {:name name
                               :path (resource/resource->proj-path spine-scene)}))
  (output resource-kind-basic-info g/Any :cached produce-spine-scene-basic-info)
  (output resource-kind-costly-info g/Any :cached produce-spine-scene-costly-info)
  (output build-errors g/Any (g/fnk [_node-id name name-counts spine-scene]
                                    (g/package-errors _node-id
                                                      (gui/prop-unique-id-error _node-id :name name name-counts "Name")
                                                      (gui/prop-resource-error _node-id :spine-scene spine-scene "Spine Scene")))))

;;//////////////////////////////////////////////////////////////////////////////////////////////

(defn- gui-scene-node->spine-resource-kind-node [basis gui-scene-node]
  (gui/gui-resource-kind-node basis gui-scene-node :spine-scene))

(defmethod ext-graph/init-attachment ::SpineSceneNode [evaluation-context rt project parent-node-id _ child-node-id attachment]
  (-> attachment
      (util/provide-defaults
        "name" (ext-graph/gen-gui-component-name attachment "spine_scene" gui-scene-node->spine-resource-kind-node rt parent-node-id evaluation-context))
      (ext-graph/attachment->set-tx-steps child-node-id rt project evaluation-context)))

(defn- fixup-spine-node [node-type-info node-desc]
  (let [type (:type node-desc)
        legacy-spine-node (identical? :type-spine type)
        template-node-child (:template-node-child node-desc)
        overridden-fields (set (:overridden-fields node-desc))
        custom-prop-kw->info (:custom-prop-kw->info node-type-info)
        custom-property-ids (into #{} (keep :id) (:custom-properties node-desc))
        custom-properties
        (reduce
          (fn [custom-properties prop-key]
            (let [{:keys [id default] :as custom-property-info} (custom-prop-kw->info prop-key)
                  pb-field-index (gui/prop-key->pb-field-index prop-key)
                  overridden (contains? overridden-fields pb-field-index)
                  value (get node-desc prop-key default)]
              (if (or (contains? custom-property-ids id)
                      (not (or overridden
                               (and (not template-node-child)
                                    (contains? node-desc prop-key))
                               (and legacy-spine-node (= :spine-scene prop-key))
                               (not= default value))))
                custom-properties
                (conj custom-properties (custom-property-info->pb custom-property-info value)))))
          (vec (:custom-properties node-desc))
          legacy-spine-prop-keys)
        custom-properties (vec (sort-by :id custom-properties))
        strip-overridden-fields (coll/any? legacy-spine-pb-field-indices overridden-fields)]
    (cond-> (cond-> (-> node-desc
                        (assoc :size-mode :size-mode-auto)
                        (dissoc
                          :spine-scene
                          :spine-default-animation
                          :spine-skin
                          :spine-create-bones
                          :spine-node-child)
                        (protobuf/assign-repeated :custom-properties custom-properties))
                    strip-overridden-fields
                    (strip-legacy-spine-overridden-fields))

            (= :type-spine type)
            (assoc
              :type (:output-type node-type-info)
              :custom-type (:output-custom-type node-type-info)))))

(node-types/register-node-type-name! SpineNode "gui-node-type-spine")

(defn- register-gui-resource-types! [workspace]
  (let [info {:node-type SpineNode
              :display-name "Spine"
              :custom-type-name "Spine"
              :icon spineext/spine-scene-icon
              :convert-fn fixup-spine-node
              :defaults gui/visual-base-node-defaults}
        legacy-info (merge info {:type :type-spine
                                 :custom-type 0
                                 :output-type :type-custom
                                 :output-custom-type (murmur/hash32 "Spine")
                                 :deprecated true})]
    (g/transact
      (concat
        (gui/register-custom-node-type-info workspace info)
        ;; Register :type-spine with custom type 0 in order to be able to read old files.
        (gui/register-node-type-info workspace legacy-info)
        (gui/register-node-tree-attachment-node-type workspace SpineNode)
        (gui/register-gui-resource-kind
          workspace :spine-scene
          {:label "Spine Scenes"
           :icon spineext/spine-scene-icon
           :exts [spineext/spine-scene-ext]
           :node-type SpineSceneNode
           :resource-property :spine-scene
           :attachment-property :spine-scenes
           :attach-fn gui/connect-gui-resource-kind-entry})))))

; The plugin
(defn load-plugin-spine-gui [workspace]
  (register-gui-resource-types! workspace))

(defn return-plugin []
  (fn [x] (load-plugin-spine-gui x)))
(return-plugin)
