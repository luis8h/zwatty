use std::io;

use crossterm::event::{self, Event, KeyCode, KeyEvent, KeyEventKind};
use libzwatty::ftms::BleWrapper;
use ratatui::{
    DefaultTerminal, Frame,
    buffer::Buffer,
    layout::Rect,
    widgets::{Block, List, Widget},
};

fn main() -> io::Result<()> {
    ratatui::run(|terminal| App::default().run(terminal))
}

#[derive(Debug, Default)]
pub struct App {
    exit: bool,
    devices: Vec<String>,
}

impl App {
    fn run(&mut self, terminal: &mut DefaultTerminal) -> io::Result<()> {
        self.devices = vec!["Hallo".into(), "from".into(), "Zwatty".into()];

        let ble = BleWrapper::try_new().await?;

        while !self.exit {
            terminal.draw(|frame| self.draw(frame))?;

            self.handle_events()?;
        }

        Ok(())
    }

    fn draw(&self, frame: &mut Frame) {
        frame.render_widget(self, frame.area());
    }

    fn handle_events(&mut self) -> io::Result<()> {
        match event::read()? {
            Event::Key(key_event) if key_event.kind == KeyEventKind::Press => {
                self.handle_key_event(key_event)
            }
            _ => {}
        }

        Ok(())
    }

    fn handle_key_event(&mut self, key_event: KeyEvent) {
        match key_event.code {
            KeyCode::Char('q') => self.exit(),
            _ => {}
        }
    }
    fn exit(&mut self) {
        self.exit = true;
    }
}

impl Widget for &App {
    fn render(self, area: Rect, buf: &mut Buffer)
    where
        Self: Sized,
    {
        List::new(self.devices.iter().map(|str| str.as_str()))
            .block(Block::bordered().title("Devices"))
            .render(area, buf);
    }
}
