#!/usr/bin/env python3
"""
Simplified 184K Context Test
Generates a large prompt file and measures engine behavior
"""

import sys
from pathlib import Path

def generate_large_prompt(target_tokens=184_313):
    """Generate a prompt with approximately target_tokens"""
    # Base paragraph with diverse vocab (~100 tokens)
    para = """The quick brown fox jumps over the lazy dog. This pangram contains most letters of the alphabet and is commonly used in typography testing. Programming languages like Python, JavaScript, C++, and Rust each have unique syntax and paradigms. Machine learning models process vast amounts of data to identify patterns and make predictions. Natural language processing enables computers to understand human communication."""
    
    # Estimate tokens (rough: 1 token per 4 characters)
    tokens_per_para = len(para) // 4
    repetitions = (target_tokens // tokens_per_para) + 1
    
    # Build document with section markers
    sections = []
    for i in range(repetitions):
        sections.append(f"[Section {i:05d}] {para}")
    
    document = "\n\n".join(sections)
    
    # Full prompt
    prompt = f"""You are a helpful assistant. Read this document and answer the question.

DOCUMENT START:
{document}
DOCUMENT END

Question: What programming languages are mentioned in the document? List them."""
    
    return prompt

def main():
    print("Generating 184K-token prompt...")
    prompt_184k = generate_large_prompt(184_313)
    
    print(f"Generated {len(prompt_184k)} characters")
    print(f"Estimated ~{len(prompt_184k) // 4} tokens")
    
    # Write to file
    output_file = Path("prompt_184k.txt")
    output_file.write_text(prompt_184k)
    print(f"Saved to: {output_file}")
    
    # Also generate 2K baseline
    prompt_2k = generate_large_prompt(2_048)
    baseline_file = Path("prompt_2k.txt")
    baseline_file.write_text(prompt_2k)
    print(f"Baseline saved to: {baseline_file}")
    
    print("\nPrompt files ready. Use with manual engine testing.")
    return 0

if __name__ == '__main__':
    sys.exit(main())
