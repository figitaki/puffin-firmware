import Foundation

/// Discover only complete assets published by this repository, including prereleases.
struct GitHubRelease: Decodable {
    static let feedURL = URL(string: "https://api.github.com/repos/figitaki/puffin-firmware/releases?per_page=100")!
    let tagName: String
    let draft: Bool
    let prerelease: Bool
    let publishedAt: Date?
    private let assets: [Asset]

    private struct Asset: Decodable {
        let name: String
        let state: String
        let size: Int
        let browserDownloadURL: URL
        enum CodingKeys: String, CodingKey {
            case name, state, size
            case browserDownloadURL = "browser_download_url"
        }
    }
    enum CodingKeys: String, CodingKey {
        case draft, prerelease, assets
        case tagName = "tag_name"
        case publishedAt = "published_at"
    }

    var version: String { tagName.hasPrefix("v") ? String(tagName.dropFirst()) : tagName }
    var manifestURL: URL { downloadURL("puffin-ProS3-manifest.json") }
    private var imageName: String { "puffin-ProS3-\(version).bin" }

    static func newestReady(in bytes: Data) throws -> GitHubRelease {
        guard bytes.count <= 2 * 1024 * 1024 else { throw UpdateFailure("The release list is too large.") }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let releases = try decoder.decode([Self].self, from: bytes)
        let allowed = CharacterSet(charactersIn: "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._-")
        let ready = releases.filter { release in
            !release.draft && release.publishedAt != nil && !release.version.isEmpty && release.version.count <= 40 &&
            release.version.unicodeScalars.allSatisfy { allowed.contains($0) } &&
            release.asset(release.imageName) != nil && release.asset("puffin-ProS3-manifest.json") != nil &&
            release.asset("puffin-ProS3-\(release.version).sha256") != nil
        }
        guard let latest = ready.max(by: { $0.publishedAt! < $1.publishedAt! }) else {
            throw UpdateFailure("No ready ProS3 firmware release is available yet. Please try again later.")
        }
        return latest
    }

    func decodeManifest(_ bytes: Data) throws -> FirmwareRelease {
        let release = try FirmwareRelease.decode(bytes)
        guard release.version == version, release.url == downloadURL(imageName),
              release.size == asset(imageName)?.size else {
            throw UpdateFailure("The update information doesn’t match the selected GitHub release.")
        }
        return release
    }

    private func downloadURL(_ name: String) -> URL {
        URL(string: "https://github.com/figitaki/puffin-firmware/releases/download")!
            .appendingPathComponent(tagName).appendingPathComponent(name)
    }

    private func asset(_ name: String) -> Asset? {
        assets.first { $0.name == name && $0.state == "uploaded" && $0.browserDownloadURL == downloadURL(name) }
    }
}

/// Publisher-controlled manifest, fetched over system-validated HTTPS.
/// Its digest binds the download to the manifest; it is not a firmware signature.
struct FirmwareRelease: Decodable {
    let version: String
    let notes: [String]
    let target: String
    let url: URL
    let size: Int
    let sha256: String

    static func decode(_ bytes: Data) throws -> FirmwareRelease {
        guard bytes.count <= 65536 else { throw UpdateFailure("The update information is too large.") }
        let release = try JSONDecoder().decode(Self.self, from: bytes)
        guard !release.version.isEmpty, release.version.count <= 40,
              release.target == "ProS3", (33...FirmwareImage.maximumSize).contains(release.size),
              release.sha256.count == 64,
              release.sha256.utf8.allSatisfy({ (48...57).contains($0) || (97...102).contains($0) }),
              release.url.scheme == "https", release.url.host != nil,
              release.url.user == nil, release.url.password == nil,
              release.notes.count <= 12, release.notes.allSatisfy({ $0.count <= 300 }) else {
            throw UpdateFailure("This update isn’t compatible with Puffin.")
        }
        return release
    }

    func validate(_ image: FirmwareImage) throws {
        guard image.bytes.count == size, image.sha256 == sha256 else {
            throw UpdateFailure("The download couldn’t be verified. Please download the update again.")
        }
    }

    static func checkResponse(_ response: URLResponse) throws {
        if let response = response as? HTTPURLResponse,
           response.statusCode == 429 || (response.statusCode == 403 && response.value(forHTTPHeaderField: "X-RateLimit-Remaining") == "0") {
            throw UpdateFailure("GitHub’s release service is rate-limited. Try again later, or install a local firmware file.")
        }
        guard let response = response as? HTTPURLResponse, response.statusCode == 200,
              response.url?.scheme == "https" else {
            throw UpdateFailure("The update service is unavailable. Please try again later.")
        }
    }
}
