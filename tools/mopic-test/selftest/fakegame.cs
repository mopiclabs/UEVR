// Stand-in "game" for the run-test.ps1 self-test (compiled by selftest.ps1 into its work folder). The harness starts
// it without arguments, so it reads what to do from the environment it inherits:
//   MOPIC_FAKE_GAME        exit:<s>:<hex code>  exit after <s> seconds with that code (a crash: C0000005)
//                          window:<s>           an invisible window for <s> seconds, then exit 0
//                          freeze:<s>           an invisible window whose UI thread blocks for <s> seconds
//                          noclose:<s>          an invisible window that refuses WM_CLOSE for <s> seconds
//   MOPIC_FAKE_GAME_TOUCH  a file to append "autosave" to right after the start (the game writing its save)
using System;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Threading;
using System.Windows.Forms;

// no taskbar button, but still an unowned top-level window (Process.MainWindowHandle skips owned ones, which is what
// ShowInTaskbar = false would make it)
public class ToolForm : Form
{
    protected override CreateParams CreateParams
    {
        get { CreateParams cp = base.CreateParams; cp.ExStyle |= 0x80; return cp; }   // WS_EX_TOOLWINDOW
    }
}

public static class FakeGame
{
    [STAThread]
    public static int Main()
    {
        string mode = Environment.GetEnvironmentVariable("MOPIC_FAKE_GAME") ?? "exit:3:0";
        string[] p = mode.Split(':');
        int secs = int.Parse(p[1], CultureInfo.InvariantCulture);
        string touch = Environment.GetEnvironmentVariable("MOPIC_FAKE_GAME_TOUCH");
        if (!string.IsNullOrEmpty(touch))
        {
            Thread.Sleep(500);
            File.AppendAllText(touch, "autosave");
        }
        if (p[0] == "exit")
        {
            Thread.Sleep(secs * 1000);
            Environment.Exit(unchecked((int)uint.Parse(p[2], NumberStyles.HexNumber, CultureInfo.InvariantCulture)));
        }
        var form = new ToolForm();
        form.Text = "mopic selftest fake game";
        form.StartPosition = FormStartPosition.Manual;
        form.Location = new Point(-32000, -32000);
        form.Size = new Size(240, 160);   // gamepilot only takes windows over 100x100
        form.Opacity = 0.0;
        DateTime until = DateTime.Now.AddSeconds(secs);
        if (p[0] == "freeze")
        {
            form.Shown += (s, e) => Thread.Sleep(secs * 1000);
        }
        if (p[0] == "noclose")
        {
            form.FormClosing += (s, e) => { if (DateTime.Now < until) e.Cancel = true; };
        }
        var timer = new System.Windows.Forms.Timer();
        timer.Interval = 200;
        timer.Tick += (s, e) => { if (DateTime.Now >= until) Environment.Exit(0); };
        timer.Start();
        Application.Run(form);
        return 0;
    }
}
