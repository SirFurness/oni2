open Exthost_Extension;

open Exthost;
open Exthost_TestLib;
open Oni_Core;

module Log = (val Timber.Log.withNamespace("Test"));

module InitData = Extension.InitData;

type t = {
  client: Client.t,
  extensionProcess: Luv.Process.t,
  processHasExited: ref(bool),
  messages: ref(list(Msg.t)),
};

let noopHandler = _ => Lwt.return(Reply.okEmpty);
let noopErrorHandler = _ => ();

let getExtensionManifest =
    (
      ~rootPath=Rench.Path.join(Sys.getcwd(), "test/collateral/extensions"),
      name,
    ) => {
  name
  |> Rench.Path.join(rootPath)
  |> (
    p =>
      Rench.Path.join(p, "package.json")
      |> Scanner.load(~category=Scanner.Bundled)
      |> Option.map(InitData.Extension.ofScanResult)
      |> Option.get
  );
};

let startWithExtensions =
    (
      ~rootPath=Rench.Path.join(Sys.getcwd(), "test/collateral/extensions"),
      ~initialConfiguration=Exthost.Configuration.empty,
      ~pid=Luv.Pid.getpid(),
      ~handler=noopHandler,
      ~onError=noopErrorHandler,
      extensions,
    ) => {
  let messages = ref([]);

  let errorHandler = err => {
    prerr_endline("ERROR: " ++ err);
    onError(err);
  };

  let wrappedHandler = msg => {
    prerr_endline("Received msg: " ++ Msg.show(msg));
    messages := [msg, ...messages^];
    handler(msg);
  };

  Timber.App.enable();

  let extensions =
    extensions
    |> List.map(Rench.Path.join(rootPath))
    |> List.map(p => Rench.Path.join(p, "package.json"))
    |> List.map(Scanner.load(~category=Scanner.Bundled))
    |> List.filter_map(v => v)
    |> List.map(InitData.Extension.ofScanResult);

  extensions |> List.iter(m => m |> InitData.Extension.show |> prerr_endline);

  let logsLocation = Filename.temp_file("test", "log") |> Uri.fromPath;
  let logFile = Filename.get_temp_dir_name() |> Uri.fromPath;

  let parentPid = pid;

  let initData =
    InitData.create(
      ~version="9.9.9",
      ~parentPid,
      ~logsLocation,
      ~logFile,
      extensions,
    );
  let pipe = NamedPipe.create("pipe-for-test");
  let pipeStr = NamedPipe.toString(pipe);
  let client =
    Client.start(
      ~initialConfiguration,
      ~namedPipe=pipe,
      ~initData,
      ~handler=wrappedHandler,
      ~onError=errorHandler,
      (),
    )
    |> ResultEx.tap_error(msg => prerr_endline(msg))
    |> Result.get_ok;

  let processHasExited = ref(false);

  let onExit = (_, ~exit_status: int64, ~term_signal: int) => {
    prerr_endline(
      Printf.sprintf(
        "Process exited: %d signal: %d",
        Int64.to_int(exit_status),
        term_signal,
      ),
    );
    processHasExited := true;
  };

  let extHostScriptPath = Setup.getNodeExtensionHostPath(Setup.init());

  let extensionProcess =
    Node.spawn(
      ~env=[
        ("PATH", Oni_Core.ShellUtility.getPathFromEnvironment()),
        (
          "AMD_ENTRYPOINT",
          "vs/workbench/services/extensions/node/extensionHostProcess",
        ),
        ("VSCODE_IPC_HOOK_EXTHOST", pipeStr),
        ("VSCODE_PARENT_PID", parentPid |> string_of_int),
      ],
      ~onExit,
      [extHostScriptPath],
    );
  {client, extensionProcess, processHasExited, messages};
};

let close = context => {
  Client.close(context.client);
  context;
};

let activateByEvent = (~event, context) => {
  Request.ExtensionService.activateByEvent(~event, context.client);
  context;
};

let executeContributedCommand = (~command, context) => {
  Request.Commands.executeContributedCommand(
    ~arguments=[],
    ~command,
    context.client,
  );
  context;
};

let waitForProcessClosed = ({processHasExited, _}) => {
  Waiter.wait(~timeout=15.0, ~name="Wait for node process to close", () =>
    processHasExited^
  );
};

let waitForMessage = (~name, f, {messages, _} as context) => {
  Waiter.wait(~name="Wait for message: " ++ name, () =>
    List.exists(f, messages^)
  );

  context;
};
let waitForReady = context => {
  waitForMessage(~name="Ready", msg => msg == Ready, context);
};

let waitForInitialized = context => {
  waitForMessage(~name="Initialized", msg => msg == Initialized, context);
};

let waitForExtensionActivation = (expectedExtensionId, context) => {
  let waitForActivation =
    fun
    | Msg.ExtensionService(DidActivateExtension({extensionId, _})) =>
      String.equal(extensionId, expectedExtensionId)
    | _ => false;

  context
  |> waitForMessage(
       ~name="Activation:" ++ expectedExtensionId,
       waitForActivation,
     );
};

let withClient = (f, context) => {
  f(context.client);
  context;
};

let withClientRequest = (~name, ~validate, f, context) => {
  let response = f(context.client);
  let hasValidated = ref(false);

  let validator = returnValue => {
    if (!validate(returnValue)) {
      failwith("Validation failed: " ++ name);
    };
    hasValidated := true;
  };
  let () = Lwt.on_success(response, validator);
  Waiter.wait(
    ~timeout=10.0,
    ~name="Waiter: " ++ name,
    () => {
      prerr_endline("Waiting...");
      hasValidated^;
    },
  );

  context;
};

let activate = (~extensionId, ~reason, context) => {
  context
  |> withClientRequest(
       ~name="Activating extension: " ++ extensionId,
       ~validate=v => v,
       Exthost.Request.ExtensionService.activate(~extensionId, ~reason),
     );
};

let validateNoPendingRequests = context => {
  if (Client.Testing.getPendingRequestCount(context.client) > 0) {
    failwith("There are still pending requests");
  };
  context;
};

let terminate = context => {
  Client.terminate(context.client);
  context;
};
