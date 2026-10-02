cask "procyon" do
  version "@VERSION@"
  sha256 "@SHA256@"

  url "https://github.com/manhpham90vn/Procyon/releases/download/v#{version}/Procyon-#{version}.dmg"
  name "Procyon"
  desc "Lightweight task manager"
  homepage "https://github.com/manhpham90vn/Procyon"

  livecheck do
    url :url
    strategy :github_latest
  end

  depends_on macos: :sonoma

  app "Procyon.app"

  uninstall quit: "dev.procyon.app"

  zap trash: [
    "~/Library/Application Support/Procyon",
    "~/Library/Preferences/dev.procyon.app.plist",
    "~/Library/Saved Application State/dev.procyon.app.savedState",
  ]
end
