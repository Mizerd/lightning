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
      # Drop keys the app owns; writing them from Nix would corrupt state.
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

      # Merge each setting into the existing file with one `initool` call per key.
      activation = lib.mkIf (cfg.settings != { }) {
        lightningMatrixClientMergeSettings = lib.hm.dag.entryAfter [ "writeBoundary" ] ''
          run mkdir -p "$(dirname "${cfg.settingsFilePath}")"
          if [ ! -f "${cfg.settingsFilePath}" ]; then
            run touch "${cfg.settingsFilePath}"
          fi
          ${
            let
              # [section ... key value] lists; unsupported value types are dropped.
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
              setSetting = ''${lib.getExe pkgs.initool} set "${cfg.settingsFilePath}"'';
              # initool prints the result instead of editing in place.
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
