// Renders the Procyon app icon to an .icns file.
//   swift scripts/make-icon.swift apps/macos/Resources/AppIcon.icns
import AppKit

let output = CommandLine.arguments.dropFirst().first ?? "AppIcon.icns"

func render(size: CGFloat) -> NSBitmapImageRep {
    let rep = NSBitmapImageRep(
        bitmapDataPlanes: nil, pixelsWide: Int(size), pixelsHigh: Int(size), bitsPerSample: 8,
        samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB,
        bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let ctx = NSGraphicsContext.current!.cgContext
    let s = size / 1024

    // macOS icon grid: 824pt squircle centred in 1024.
    let rect = CGRect(x: 100 * s, y: 100 * s, width: 824 * s, height: 824 * s)
    let shape = NSBezierPath(roundedRect: rect, xRadius: 185 * s, yRadius: 185 * s)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -10 * s), blur: 28 * s, color: NSColor.black.withAlphaComponent(0.35).cgColor)
    NSColor.black.setFill()
    shape.fill()
    ctx.restoreGState()

    shape.addClip()
    NSGradient(colors: [
        NSColor(red: 0.07, green: 0.08, blue: 0.16, alpha: 1), NSColor(red: 0.16, green: 0.10, blue: 0.32, alpha: 1),
    ])!
    .draw(in: rect, angle: 60)
    // Nebula glow.
    NSGradient(colors: [NSColor(red: 0.23, green: 0.51, blue: 0.96, alpha: 0.55), .clear])!
        .draw(
            fromCenter: CGPoint(x: 360 * s, y: 640 * s), radius: 0, toCenter: CGPoint(x: 360 * s, y: 640 * s), radius: 520 * s,
            options: [])
    NSGradient(colors: [NSColor(red: 0.93, green: 0.28, blue: 0.60, alpha: 0.40), .clear])!
        .draw(
            fromCenter: CGPoint(x: 760 * s, y: 300 * s), radius: 0, toCenter: CGPoint(x: 760 * s, y: 300 * s), radius: 460 * s,
            options: [])

    // Live chart line.
    let points: [CGPoint] = [
        (150, 380), (260, 400), (340, 340), (420, 470), (500, 420), (580, 560), (650, 500), (730, 610), (874, 590),
    ]
    .map { CGPoint(x: $0.0 * s, y: $0.1 * s) }
    let line = CGMutablePath()
    line.move(to: points[0])
    for i in 1..<points.count {
        let mid = CGPoint(x: (points[i - 1].x + points[i].x) / 2, y: (points[i - 1].y + points[i].y) / 2)
        line.addQuadCurve(to: mid, control: points[i - 1])
    }
    line.addLine(to: points.last!)

    let area = line.mutableCopy()!
    area.addLine(to: CGPoint(x: 874 * s, y: 100 * s))
    area.addLine(to: CGPoint(x: 150 * s, y: 100 * s))
    area.closeSubpath()
    ctx.saveGState()
    ctx.addPath(area)
    ctx.clip()
    let fill = CGGradient(
        colorsSpace: CGColorSpaceCreateDeviceRGB(),
        colors: [
            NSColor(red: 0.13, green: 0.83, blue: 0.93, alpha: 0.45).cgColor,
            NSColor(red: 0.13, green: 0.83, blue: 0.93, alpha: 0).cgColor,
        ] as CFArray, locations: [0, 1])!
    ctx.drawLinearGradient(fill, start: CGPoint(x: 0, y: 620 * s), end: CGPoint(x: 0, y: 120 * s), options: [])
    ctx.restoreGState()

    ctx.saveGState()
    ctx.setShadow(offset: .zero, blur: 30 * s, color: NSColor(red: 0.13, green: 0.83, blue: 0.93, alpha: 0.9).cgColor)
    ctx.addPath(line)
    ctx.setLineWidth(26 * s)
    ctx.setLineCap(.round)
    ctx.setLineJoin(.round)
    ctx.replacePathWithStrokedPath()
    ctx.clip()
    let stroke = CGGradient(
        colorsSpace: CGColorSpaceCreateDeviceRGB(),
        colors: [
            NSColor(red: 0.23, green: 0.51, blue: 0.96, alpha: 1).cgColor,
            NSColor(red: 0.13, green: 0.83, blue: 0.93, alpha: 1).cgColor,
            NSColor(red: 0.93, green: 0.28, blue: 0.60, alpha: 1).cgColor,
        ] as CFArray, locations: [0, 0.55, 1])!
    ctx.drawLinearGradient(stroke, start: CGPoint(x: 150 * s, y: 0), end: CGPoint(x: 874 * s, y: 0), options: [])
    ctx.restoreGState()

    // Procyon: the bright star.
    let star = CGPoint(x: 700 * s, y: 760 * s)
    NSGradient(colors: [NSColor.white.withAlphaComponent(0.9), NSColor(red: 0.6, green: 0.8, blue: 1, alpha: 0.25), .clear])!
        .draw(fromCenter: star, radius: 0, toCenter: star, radius: 120 * s, options: [])
    let sparkle = NSBezierPath()
    let arm: CGFloat = 92 * s, waist: CGFloat = 14 * s
    sparkle.move(to: CGPoint(x: star.x, y: star.y + arm))
    sparkle.curve(
        to: CGPoint(x: star.x + arm, y: star.y), controlPoint1: CGPoint(x: star.x + waist, y: star.y + waist),
        controlPoint2: CGPoint(x: star.x + waist, y: star.y + waist))
    sparkle.curve(
        to: CGPoint(x: star.x, y: star.y - arm), controlPoint1: CGPoint(x: star.x + waist, y: star.y - waist),
        controlPoint2: CGPoint(x: star.x + waist, y: star.y - waist))
    sparkle.curve(
        to: CGPoint(x: star.x - arm, y: star.y), controlPoint1: CGPoint(x: star.x - waist, y: star.y - waist),
        controlPoint2: CGPoint(x: star.x - waist, y: star.y - waist))
    sparkle.curve(
        to: CGPoint(x: star.x, y: star.y + arm), controlPoint1: CGPoint(x: star.x - waist, y: star.y + waist),
        controlPoint2: CGPoint(x: star.x - waist, y: star.y + waist))
    NSColor.white.setFill()
    sparkle.fill()

    // Hairline highlight.
    NSColor.white.withAlphaComponent(0.12).setStroke()
    shape.lineWidth = 4 * s
    shape.stroke()

    NSGraphicsContext.restoreGraphicsState()
    return rep
}

let iconset = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("Procyon.iconset")
try? FileManager.default.removeItem(at: iconset)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
for base in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let name = scale == 1 ? "icon_\(base)x\(base).png" : "icon_\(base)x\(base)@2x.png"
        try render(size: CGFloat(base * scale)).representation(using: .png, properties: [:])!
            .write(to: iconset.appendingPathComponent(name))
    }
}
let process = Process()
process.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
process.arguments = ["-c", "icns", iconset.path, "-o", output]
try process.run()
process.waitUntilExit()
try render(size: 512).representation(using: .png, properties: [:])!.write(
    to: URL(fileURLWithPath: output).deletingPathExtension().appendingPathExtension("png"))
print("wrote \(output)")
