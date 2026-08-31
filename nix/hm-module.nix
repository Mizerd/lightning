inputs:
{
  config,
  lib,
  pkgs,
  ...
}:

let
  cfg = config.lightning-matrix-client;
in
{
  options.lightning-matrix-client = {
    enable = lib.mkEnableOption ''
      Lightning — a native Qt 6/QML Matrix desktop client
    '';

    package =
      lib.mkPackageOption inputs.self.packages.${pkgs.stdenv.hostPlatform.system}
        "lightning-matrix-client"
        { };

    settingsFilePath = lib.mkOption {
      type = lib.types.str;
      default = "$XDG_CONFIG_HOME/MatrixClient/matrix-client.conf";
      internal = true;
      description = ''
        Internal option for getting the settings file path for Lightning.
      '';
    };

    settings = lib.mkOption {
      type = lib.types.submodule {
        freeformType = (pkgs.formats.ini { }).type;
        options = { };
      };
      default = { };
      example = {
        notifications = {
          enabled = false;
        };
        ui = {
          clockFormat = 2;
          theme = 3;
          uiFont = "Inter";
        };
      };
      # Remove settings that may cause issues
      apply =
        attrs:
        (lib.removeAttrs
          (
            attrs
            // {
              ui = lib.removeAttrs (attrs.ui or { }) [
                "importedFonts" # runtime registry pointing at font files the app copied itself
                "roomFilterMode" # transient view state
              ];
            }
          )
          [
            "accounts" # per-account records, including the mapping from account to crypto-store path
            "session" # dead migration keys from pre-0.7. writing anything here would be actively bad
            "security" # a dismissal record, not a preference
          ]
        );
      description = ''
        Settings to be merged with the matrix-client.conf file.
      '';
    };
  };

  config = lib.mkIf cfg.enable {
    home = {
      packages = [
        cfg.package
      ];

      # Activation script that sets all settings on top of the original file (if any)
      # The easiest and fastest way I found to do this is by using `inittool` to run a separate command for each setting
      activation = lib.mkIf (cfg.settings != { }) {
        lightningMatrixClientMergeSettings = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
          run mkdir -p "$(dirname "${cfg.settingsFilePath}")"
          if [ ! -f "${cfg.settingsFilePath}" ]; then
            run touch "${cfg.settingsFilePath}"
          fi
          ${
            let
              # We need to split each setting into a list containing its path
              # and process the values to retain only the possible/valid ones
              settingsList = lib.filter (list: list != null) (
                lib.mapAttrsToListRecursive (
                  path: value:
                  let
                    valueType = lib.typeOf value;
                    processedValue =
                      if valueType == "bool" then
                        lib.boolToString value
                      else if valueType == "int" then
                        lib.toString value
                      else if valueType == "string" then
                        value
                      else if valueType == "float" then
                        lib.strings.floatToString value
                      else
                        null;
                  in
                  if lib.length path < 2 then
                    null
                  else if processedValue == null then
                    null
                  else
                    path ++ [ processedValue ]
                ) cfg.settings
              );
              # Snippets to avoid a HUGE line
              setSetting = ''${lib.getExe pkgs.initool} set "${cfg.settingsFilePath}"'';
              # We need to create a temporary file for each option
              # `initool` on its own does not replace the value inside the original file, only returns the result
              pipeReplace = ''> "${cfg.settingsFilePath}.tmppipe" && mv "${cfg.settingsFilePath}.tmppipe" "${cfg.settingsFilePath}"'';
            in
            lib.concatStringsSep "\n" (
              lib.map (setting: ''
                run ${setSetting} "${lib.head setting}" "${lib.concatStringsSep ''\\'' (lib.tail (lib.dropEnd 1 setting))}" "${lib.last setting}" ${pipeReplace}
              '') settingsList
            )
          }
        '';
      };
    };
  };
}
