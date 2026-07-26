package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"time"

	"go.bug.st/serial"
)

var (
	uuidPattern       = regexp.MustCompile(`UUID_REQUEST\s+session=(\d+)\s+node=([0-9a-fA-F:]{17})\s+uuid=(\S+)`)
	treeHeaderPattern = regexp.MustCompile(`^TREE\s+count=(\d+)\s+complete=([01])$`)
	treeNodePattern   = regexp.MustCompile(`^(\d+)\s+([0-9a-fA-F:]{17})\s+parent=([0-9a-fA-F:]{17})\s+parent_known=([01])\s+direct_child=([01])$`)
	colorPattern      = regexp.MustCompile(`^[0-9a-fA-F]{6}$`)
)

type treeNode struct {
	Index       int    `json:"index"`
	MAC         string `json:"mac"`
	Parent      string `json:"parent"`
	ParentKnown bool   `json:"parent_known"`
	DirectChild bool   `json:"direct_child"`
}

type treeSnapshot struct {
	Header *struct {
		Count    int  `json:"count"`
		Complete bool `json:"complete"`
	} `json:"header"`
	Nodes     []treeNode `json:"nodes"`
	UpdatedAt *time.Time `json:"updated_at"`
	Error     string     `json:"error"`
}

type meshState struct {
	port       serial.Port
	serialMu   sync.Mutex
	colorsMu   sync.RWMutex
	treeMu     sync.RWMutex
	colors     map[string]string
	tableFile  string
	fallback   string
	tree       treeSnapshot
	treePeriod time.Duration
	printAll   bool
}

func normalizeColor(value string) (string, error) {
	value = strings.TrimPrefix(strings.TrimSpace(value), "#")
	if !colorPattern.MatchString(value) {
		return "", errors.New("color must be #RRGGBB or RRGGBB")
	}
	return strings.ToLower(value), nil
}

func loadColors(path string) (map[string]string, error) {
	colors := make(map[string]string)
	data, err := os.ReadFile(path)
	if errors.Is(err, os.ErrNotExist) {
		return colors, nil
	}
	if err != nil {
		return nil, err
	}
	if err := json.Unmarshal(data, &colors); err != nil {
		return nil, err
	}
	for uuid, color := range colors {
		normalized, err := normalizeColor(color)
		if err != nil {
			return nil, fmt.Errorf("%s: %w", uuid, err)
		}
		colors[uuid] = normalized
	}
	return colors, nil
}

func (s *meshState) saveColors() error {
	s.colorsMu.RLock()
	data, err := json.MarshalIndent(s.colors, "", "  ")
	s.colorsMu.RUnlock()
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(s.tableFile), 0755); err != nil {
		return err
	}
	return os.WriteFile(s.tableFile, append(data, '\n'), 0644)
}

func (s *meshState) setColor(uuid, color string) (string, error) {
	normalized, err := normalizeColor(color)
	if err != nil {
		return "", err
	}
	s.colorsMu.Lock()
	s.colors[uuid] = normalized
	s.colorsMu.Unlock()
	if err := s.saveColors(); err != nil {
		return "", err
	}
	return normalized, nil
}

func (s *meshState) writeCommand(command string) error {
	s.serialMu.Lock()
	defer s.serialMu.Unlock()
	_, err := io.WriteString(s.port, command+"\r\n")
	return err
}

func (s *meshState) handleLine(line string) {
	if s.printAll {
		log.Print(line)
	}
	if match := uuidPattern.FindStringSubmatch(line); match != nil {
		s.colorsMu.RLock()
		color := s.colors[match[3]]
		if color == "" {
			color = s.fallback
		}
		s.colorsMu.RUnlock()
		log.Printf("UUID_REQUEST node=%s uuid=%s -> #%s", match[2], match[3], color)
		if err := s.writeCommand("color " + color); err != nil {
			log.Printf("send color: %v", err)
		}
	}
	if match := treeHeaderPattern.FindStringSubmatch(line); match != nil {
		count, _ := strconv.Atoi(match[1])
		now := time.Now()
		s.treeMu.Lock()
		s.tree = treeSnapshot{
			Header: &struct {
				Count    int  `json:"count"`
				Complete bool `json:"complete"`
			}{Count: count, Complete: match[2] == "1"},
			Nodes:     []treeNode{},
			UpdatedAt: &now,
		}
		s.treeMu.Unlock()
		return
	}
	if match := treeNodePattern.FindStringSubmatch(line); match != nil {
		index, _ := strconv.Atoi(match[1])
		s.treeMu.Lock()
		if s.tree.Header != nil {
			s.tree.Nodes = append(s.tree.Nodes, treeNode{
				Index: index, MAC: strings.ToLower(match[2]), Parent: strings.ToLower(match[3]),
				ParentKnown: match[4] == "1", DirectChild: match[5] == "1",
			})
		}
		s.treeMu.Unlock()
	}
}

func (s *meshState) run() error {
	reader := bufio.NewScanner(s.port)
	ticker := time.NewTicker(s.treePeriod)
	defer ticker.Stop()
	if err := s.writeCommand("TREE"); err != nil {
		return err
	}
	lines := make(chan string)
	readErrors := make(chan error, 1)
	go func() {
		for reader.Scan() {
			lines <- strings.TrimSpace(reader.Text())
		}
		readErrors <- reader.Err()
	}()
	for {
		select {
		case <-ticker.C:
			if err := s.writeCommand("TREE"); err != nil {
				log.Printf("request tree: %v", err)
			}
		case line := <-lines:
			s.handleLine(line)
		case err := <-readErrors:
			return err
		}
	}
}

type apiHandler struct{ state *meshState }

func (h *apiHandler) json(w http.ResponseWriter, status int, value any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(value)
}

func (h *apiHandler) readJSON(r *http.Request, value any) error {
	decoder := json.NewDecoder(io.LimitReader(r.Body, 1<<20))
	return decoder.Decode(value)
}

func (h *apiHandler) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	path := strings.TrimRight(r.URL.Path, "/")
	switch {
	case r.Method == http.MethodGet && path == "/api/health":
		h.json(w, http.StatusOK, map[string]bool{"ok": true})
	case r.Method == http.MethodGet && path == "/api/tree":
		h.state.treeMu.RLock()
		snapshot := h.state.tree
		h.state.treeMu.RUnlock()
		h.json(w, http.StatusOK, snapshot)
	case r.Method == http.MethodGet && path == "/api/colors":
		h.state.colorsMu.RLock()
		colors := mapsCopy(h.state.colors)
		h.state.colorsMu.RUnlock()
		h.json(w, http.StatusOK, map[string]any{"colors": colors})
	case r.Method == http.MethodPut && strings.HasPrefix(path, "/api/colors/"):
		uuid := strings.TrimPrefix(path, "/api/colors/")
		if uuid == "" {
			h.json(w, http.StatusBadRequest, map[string]string{"error": "missing uuid"})
			return
		}
		var body struct {
			Color string `json:"color"`
		}
		if err := h.readJSON(r, &body); err != nil {
			h.json(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
			return
		}
		color, err := h.state.setColor(uuid, body.Color)
		if err != nil {
			h.json(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
			return
		}
		h.json(w, http.StatusOK, map[string]string{"uuid": uuid, "color": color})
	case r.Method == http.MethodPatch && path == "/api/colors":
		var body map[string]string
		if err := h.readJSON(r, &body); err != nil {
			h.json(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
			return
		}
		updated := make(map[string]string)
		for uuid, color := range body {
			normalized, err := h.state.setColor(uuid, color)
			if err != nil {
				h.json(w, http.StatusBadRequest, map[string]string{"error": err.Error()})
				return
			}
			updated[uuid] = normalized
		}
		h.json(w, http.StatusOK, map[string]any{"updated": updated})
	default:
		h.json(w, http.StatusNotFound, map[string]string{"error": "not found"})
	}
}

func mapsCopy(input map[string]string) map[string]string {
	output := make(map[string]string, len(input))
	for key, value := range input {
		output[key] = value
	}
	return output
}

func runServer(args []string) error {
	flags := flag.NewFlagSet("server", flag.ContinueOnError)
	portName := flags.String("port", "", "root serial port")
	baud := flags.Int("baud", 115200, "serial baud rate")
	host := flags.String("host", "127.0.0.1", "HTTP bind host")
	apiPort := flags.Int("api-port", 8080, "HTTP port")
	fallback := flags.String("color", "00ff00", "fallback color")
	tableFile := flags.String("table-file", "uuid_colors.json", "UUID color JSON file")
	interval := flags.Duration("tree-interval", 5*time.Second, "tree request interval")
	printAll := flags.Bool("print-all", false, "print every serial line")
	if err := flags.Parse(args); err != nil {
		return err
	}
	if *portName == "" {
		return errors.New("--port is required")
	}
	color, err := normalizeColor(*fallback)
	if err != nil {
		return err
	}
	colors, err := loadColors(*tableFile)
	if err != nil {
		return err
	}
	serialPort, err := serial.Open(*portName, &serial.Mode{BaudRate: *baud})
	if err != nil {
		return err
	}
	defer serialPort.Close()
	state := &meshState{port: serialPort, colors: colors, tableFile: *tableFile, fallback: color, treePeriod: *interval, printAll: *printAll,
		tree: treeSnapshot{Nodes: []treeNode{}, Error: "not received"}}
	go func() {
		if err := state.run(); err != nil {
			log.Printf("serial watcher stopped: %v", err)
		}
	}()
	handler := &apiHandler{state: state}
	log.Printf("API listening on http://%s:%d", *host, *apiPort)
	return http.ListenAndServe(fmt.Sprintf("%s:%d", *host, *apiPort), handler)
}

func apiRequest(base, method, path string, body any) (any, error) {
	var reader io.Reader
	if body != nil {
		data, err := json.Marshal(body)
		if err != nil {
			return nil, err
		}
		reader = strings.NewReader(string(data))
	}
	req, err := http.NewRequest(method, strings.TrimRight(base, "/")+path, reader)
	if err != nil {
		return nil, err
	}
	if body != nil {
		req.Header.Set("Content-Type", "application/json")
	}
	response, err := http.DefaultClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer response.Body.Close()
	var result any
	if err := json.NewDecoder(response.Body).Decode(&result); err != nil {
		return nil, err
	}
	if response.StatusCode >= 300 {
		return nil, fmt.Errorf("HTTP %s: %v", response.Status, result)
	}
	return result, nil
}

func runClient(args []string) error {
	if len(args) == 0 {
		return errors.New("client command is required: color, colors, or tree")
	}
	base := "http://127.0.0.1:8080"
	commandArgs := make([]string, 0, len(args))
	for index := 0; index < len(args); index++ {
		if args[index] == "--url" {
			if index+1 >= len(args) {
				return errors.New("--url requires a value")
			}
			base = args[index+1]
			index++
			continue
		}
		if strings.HasPrefix(args[index], "--url=") {
			base = strings.TrimPrefix(args[index], "--url=")
			continue
		}
		commandArgs = append(commandArgs, args[index])
	}
	if len(commandArgs) == 0 {
		return errors.New("client command is required: color, colors, or tree")
	}
	var method, path string
	var body any
	switch commandArgs[0] {
	case "color":
		if len(commandArgs) != 3 {
			return errors.New("usage: color <uuid> <RRGGBB>")
		}
		method, path, body = http.MethodPut, "/api/colors/"+url.PathEscape(commandArgs[1]), map[string]string{"color": commandArgs[2]}
	case "colors":
		if len(commandArgs) != 2 {
			return errors.New(`usage: colors '{"uuid":"ff0000"}'`)
		}
		if err := json.Unmarshal([]byte(commandArgs[1]), &body); err != nil {
			return err
		}
		method, path = http.MethodPatch, "/api/colors"
	case "tree":
		if len(commandArgs) != 1 {
			return errors.New("usage: tree")
		}
		method, path = http.MethodGet, "/api/tree"
	default:
		return fmt.Errorf("unknown client command %q", commandArgs[0])
	}
	result, err := apiRequest(base, method, path, body)
	if err != nil {
		return err
	}
	data, _ := json.MarshalIndent(result, "", "  ")
	fmt.Println(string(data))
	return nil
}

func main() {
	if len(os.Args) < 2 {
		log.Fatal("usage: ss-mesh server ... | client ...")
	}
	var err error
	switch os.Args[1] {
	case "server":
		err = runServer(os.Args[2:])
	case "client":
		err = runClient(os.Args[2:])
	default:
		err = fmt.Errorf("unknown command %q", os.Args[1])
	}
	if err != nil {
		log.Fatal(err)
	}
}
