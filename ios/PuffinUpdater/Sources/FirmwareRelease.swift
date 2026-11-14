import Foundation

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
        guard let response = response as? HTTPURLResponse, response.statusCode == 200,
              response.url?.scheme == "https" else {
            throw UpdateFailure("The update service is unavailable. Please try again later.")
        }
    }
}
