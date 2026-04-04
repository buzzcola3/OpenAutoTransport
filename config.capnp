@0xe3c2290df4118fed;

# ── Supporting structs ────────────────────────────────────────────────

struct KeyValue {
  key   @0 :Text;
  value @1 :Text;
}

struct FieldOption {
  label @0 :Text;
  value @1 :Text;
}

struct ChannelFieldInfo {
  name    @0 :Text;
  value   @1 :Text;
  options @2 :List(FieldOption);  # empty if free-form
}

struct ChannelInfo {
  id      @0 :UInt32;
  kind    @1 :Text;
  summary @2 :Text;
  fields  @3 :List(ChannelFieldInfo);
}

struct ScalarField {
  name    @0 :Text;
  value   @1 :Text;
  options @2 :List(FieldOption);
}

struct AddableParam {
  name    @0 :Text;
  options @1 :List(FieldOption);
  type    @2 :Text;   # e.g. "enum", "uint32", "text"
}

struct AddableType {
  kind        @0 :Text;
  params      @1 :List(AddableParam);
  label       @2 :Text;   # human-readable name
  description @3 :Text;   # longer descriptive text
}

struct ValidationError {
  field   @0 :Text;
  message @1 :Text;
}

struct ConfigStatus {
  customFileExists     @0 :Bool;
  customFilePath       @1 :Text;
  divergesFromDefaults @2 :Bool;
}

# ── ConfigService RPC ─────────────────────────────────────────────────

interface ConfigService {
  # Introspection
  getChannels            @0 ()                       -> (channels :List(ChannelInfo));
  getChannel             @1 (channelId :UInt32)      -> (channel :ChannelInfo);
  getScalars             @2 ()                       -> (scalars :List(ScalarField));
  getAddableChannelTypes @3 ()                       -> (types :List(AddableType));
  getDefaultState        @4 ()                       -> (textproto :Text);
  getCurrentState        @5 ()                       -> (textproto :Text);
  getStatus              @6 ()                       -> (status :ConfigStatus);

  # Channel mutations
  setChannelField  @7  (channelId :UInt32, fieldName :Text, value :Text)
                       -> (success :Bool, error :Text);
  addChannel       @8  (channelKind :Text, params :List(KeyValue))
                       -> (success :Bool, newChannelId :UInt32, error :Text);
  removeChannel    @9  (channelId :UInt32)
                       -> (success :Bool, error :Text);
  replaceChannel   @10 (channelId :UInt32, channelKind :Text, params :List(KeyValue))
                       -> (success :Bool, error :Text);
  reorderChannels  @11 (channelIdOrder :List(UInt32))
                       -> (success :Bool, error :Text);

  # Scalar mutations
  setScalar        @12 (fieldName :Text, value :Text)
                       -> (success :Bool, error :Text);
  setScalars       @13 (fields :List(KeyValue))
                       -> (success :Bool, failedFields :List(Text), error :Text);
  setPingConfig    @14 (timeoutMs :UInt32, intervalMs :UInt32,
                        hiLatencyThresholdMs :UInt32, trackedCount :UInt32)
                       -> (success :Bool, error :Text);
  setHeadunitInfo  @15 (make :Text, model :Text, year :Text, vehicleId :Text,
                        huMake :Text, huModel :Text, swBuild :Text, swVersion :Text)
                       -> (success :Bool, error :Text);

  # Persistence
  save             @16 () -> (success :Bool, error :Text, path :Text);
  resetToDefaults  @17 () -> (success :Bool, error :Text);
  discardUnsaved   @18 () -> (success :Bool);
  exportConfig     @19 () -> (textproto :Text);
  importConfig     @20 (textproto :Text) -> (success :Bool, error :Text);

  # Validation
  validateChannelField @21 (channelId :UInt32, fieldName :Text, value :Text)
                           -> (valid :Bool, error :Text);
  validateAddChannel   @22 (channelKind :Text, params :List(KeyValue))
                           -> (valid :Bool, error :Text);
  validateConfig       @23 () -> (valid :Bool, errors :List(ValidationError));

  # Push subscription
  subscribe        @24 (observer :ConfigObserver) -> (handle :SubscriptionHandle);
}

# ── Push notifications (core → UI) ───────────────────────────────────

interface ConfigObserver {
  configChanged  @0 (changedFields :List(Text)) -> ();
  configSaved    @1 (path :Text)                -> ();
  configReset    @2 ()                          -> ();
  sessionStarted @3 ()                          -> ();
  sessionEnded   @4 ()                          -> ();
}

interface SubscriptionHandle {
  # Dropping this capability cancels the subscription.
}
